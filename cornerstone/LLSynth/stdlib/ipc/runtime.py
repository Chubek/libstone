# Embedded only when std.ipc is imported.
import errno as _ipc_errno
import os as _ipc_os
import stat as _ipc_stat
import sys as _ipc_sys
from threading import Lock as _ipc_lock

_IPC_LIMIT = 1024 * 1024
_IPC_INT_MAX = (1 << 31) - 1
_IPC_SEM_LIMIT = 1024
_ipc_sem_lock = _ipc_lock()
_ipc_semaphores = {}
_ipc_next_semaphore = -1


def _ipc_unavailable(detail):
    raise LanguageError('IPC_UNAVAILABLE', detail)


def _ipc_number(value, lower, upper, label):
    if not lower <= value <= upper:
        raise ValueError(f'{label} must be in {lower}..{upper}')
    return value


def _ipc_name(name):
    if len(name) < 2 or name[0] != '/' or '/' in name[1:] or name[1:] in ('.', '..'):
        raise ValueError('expected a POSIX name of the form /name')
    return name.encode('utf-8')


def _ipc_bytes(data):
    _ipc_number(len(data), 0, _IPC_LIMIT, 'payload size')
    if any(type(value) is not int or not 0 <= value <= 255 for value in data):
        raise ValueError('expected an octet vector (integers in 0..255)')
    return bytes(data)


def _ipc_lib(family, symbols):
    if not _ipc_sys.platform.startswith('linux'):
        operation = {'mq': 'POSIX message queues', 'shm': 'POSIX shared memory with positioned I/O',
                     'sem': 'POSIX named semaphores'}[family]
        _ipc_unavailable(f'{operation} requires a Linux host')
    try:
        import ctypes
    except ImportError:
        _ipc_unavailable('the Python host does not provide ctypes')
    candidates = [None]
    if _ipc_sys.platform.startswith('linux'):
        candidates.append('librt.so.1')
    for name in candidates:
        try:
            library = ctypes.CDLL(name, use_errno=True)
            if all(hasattr(library, symbol) for symbol in symbols):
                return ctypes, library
        except OSError:
            continue
    _ipc_unavailable(f'host has no {family} API ({", ".join(symbols)})')


def _ipc_native(ctypes, library, name, result, argtypes, *args):
    fn = getattr(library, name)
    fn.restype, fn.argtypes = result, argtypes
    value = fn(*args)
    if value == -1:
        _ipc_raise_errno(ctypes.get_errno())
    return value


def _ipc_raise_errno(number):
    raise OSError(number, _ipc_os.strerror(number))


def _ipc_shm(op, args):
    ctypes, lib = _ipc_lib('shm', ('shm_open', 'shm_unlink'))
    name = _ipc_name(args[0])
    if op == 'shmUnlink':
        _ipc_native(ctypes, lib, 'shm_unlink', ctypes.c_int, [ctypes.c_char_p], name)
        return None
    create = op == 'shmCreate'
    size = _ipc_number(args[1], 0, 1 << 30, 'shared memory size') if create else 0
    flags = _ipc_os.O_RDWR if create or args[1] else _ipc_os.O_RDONLY
    if create:
        flags |= _ipc_os.O_CREAT | _ipc_os.O_EXCL
    try:
        fd = _ipc_native(ctypes, lib, 'shm_open', ctypes.c_int,
                         [ctypes.c_char_p, ctypes.c_int, ctypes.c_uint], name, flags, 0o600)
    except OSError as exc:
        if create and exc.errno == _ipc_errno.ENOENT:
            _ipc_unavailable('cannot create the POSIX shared memory object; check the host namespace (Linux: /dev/shm)')
        raise
    try:
        _ipc_os.set_inheritable(fd, False)
        if create:
            _ipc_os.ftruncate(fd, size)
    except BaseException:
        try:
            _ipc_os.close(fd)
        except OSError:
            pass
        if create:
            try:
                _ipc_native(ctypes, lib, 'shm_unlink', ctypes.c_int, [ctypes.c_char_p], name)
            except OSError:
                pass
        raise
    return fd


def _ipc_mq_api():
    return _ipc_lib('mq', ('mq_open', 'mq_close', 'mq_unlink', 'mq_getattr', 'mq_send', 'mq_receive'))


def _ipc_mq(op, args):
    ctypes, lib = _ipc_mq_api()
    cint = ctypes.c_int
    ccharp = ctypes.c_char_p
    cvoidp = ctypes.c_void_p
    csize = ctypes.c_size_t

    class Attr(ctypes.Structure):
        # Linux userspace ABI: four fields followed by four reserved longs.
        _fields_ = [('flags', ctypes.c_long), ('maxmsg', ctypes.c_long),
                    ('msgsize', ctypes.c_long), ('curmsgs', ctypes.c_long),
                    ('reserved', ctypes.c_long * 4)]

    if op in ('mqCreate', 'mqOpen', 'mqUnlink'):
        name = _ipc_name(args[0])
        if op == 'mqUnlink':
            _ipc_native(ctypes, lib, 'mq_unlink', cint, [ccharp], name)
            return None
        flags = _ipc_os.O_RDWR | getattr(_ipc_os, 'O_CLOEXEC', 0)
        if args[-1]:
            flags |= _ipc_os.O_NONBLOCK
        base = (ctypes, lib, 'mq_open', cint, [ccharp, cint], name)
        if op == 'mqOpen':
            return _ipc_native(*base, flags)
        attr = Attr(maxmsg=_ipc_number(args[1], 1, _IPC_INT_MAX, 'queue depth'),
                    msgsize=_ipc_number(args[2], 1, _IPC_LIMIT, 'maximum message size'))
        # mq_open is variadic; only the fixed parameters belong in argtypes.
        return _ipc_native(*base, flags | _ipc_os.O_CREAT | _ipc_os.O_EXCL,
                           ctypes.c_uint(0o600), ctypes.byref(attr))
    fd = _ipc_number(args[0], 0, _IPC_INT_MAX, 'queue descriptor')
    if op == 'mqClose':
        _ipc_native(ctypes, lib, 'mq_close', cint, [cint], fd)
        return None
    if op == 'mqSend':
        payload = _ipc_bytes(args[1])
        priority = _ipc_number(args[2], 0, _IPC_INT_MAX, 'priority')
        while True:
            try:
                _ipc_native(ctypes, lib, 'mq_send', cint, [cint, ccharp, csize, ctypes.c_uint],
                            fd, payload, len(payload), priority)
                return None
            except InterruptedError:
                continue
    attr = Attr()
    _ipc_native(ctypes, lib, 'mq_getattr', cint, [cint, ctypes.POINTER(Attr)], fd, ctypes.byref(attr))
    length = _ipc_number(attr.msgsize, 1, _IPC_LIMIT, 'maximum message size')
    target = ctypes.create_string_buffer(length)
    while True:
        try:
            count = _ipc_native(ctypes, lib, 'mq_receive', ctypes.c_ssize_t, [cint, cvoidp, csize, cvoidp],
                                fd, target, length, None)
            return list(target.raw[:count])
        except InterruptedError:
            continue


def _ipc_sem_api():
    return _ipc_lib('sem', ('sem_open', 'sem_close', 'sem_trywait', 'sem_post', 'sem_unlink'))


def _ipc_semaphore(op, args):
    global _ipc_next_semaphore
    if not _ipc_sys.platform.startswith('linux'):
        _ipc_unavailable('POSIX named semaphores require a Linux host')
    if op in ('semCreate', 'semOpen', 'semUnlink'):
        name = _ipc_name(args[0])
        ctypes, lib = _ipc_sem_api()
        if op == 'semUnlink':
            _ipc_native(ctypes, lib, 'sem_unlink', ctypes.c_int, [ctypes.c_char_p], name)
            return None
        create = op == 'semCreate'
        flags = (_ipc_os.O_CREAT | _ipc_os.O_EXCL) if create else 0
        value = _ipc_number(args[1], 0, _IPC_INT_MAX, 'initial semaphore value') if create else 0
        fn = lib.sem_open
        fn.restype = ctypes.c_void_p
        # sem_open is variadic; mode_t and initial value are unsigned int on Linux.
        fn.argtypes = [ctypes.c_char_p, ctypes.c_int]
        with _ipc_sem_lock:
            if len(_ipc_semaphores) >= _IPC_SEM_LIMIT or _ipc_next_semaphore == -(1 << 63):
                raise ValueError('semaphore handle limit reached')
            handle = _ipc_next_semaphore
            entry = [ctypes, lib, None]
            _ipc_semaphores[handle] = entry
            try:
                if create:
                    pointer = fn(name, flags, ctypes.c_uint(0o600), ctypes.c_uint(value))
                else:
                    pointer = fn(name, flags)
                # glibc/musl use NULL; also recognize the all-bits-one sentinel
                # used by some other Linux libcs. Never interpret it as a handle.
                if pointer is None or pointer == ctypes.c_void_p(-1).value:
                    number = ctypes.get_errno()
                    if create and number == _ipc_errno.ENOENT:
                        _ipc_unavailable('cannot create the named semaphore; check the host namespace (/dev/shm)')
                    _ipc_raise_errno(number)
                entry[2] = pointer
            except BaseException:
                del _ipc_semaphores[handle]
                raise
            _ipc_next_semaphore -= 1
            return handle
    symbol = {'semClose': 'sem_close', 'semTryWait': 'sem_trywait', 'semPost': 'sem_post'}[op]
    handle = args[0]
    with _ipc_sem_lock:
        if handle not in _ipc_semaphores:
            raise ValueError('invalid or closed semaphore handle')
        ctypes, lib, pointer = _ipc_semaphores[handle]
        while True:
            try:
                _ipc_native(ctypes, lib, symbol, ctypes.c_int, [ctypes.c_void_p], pointer)
                break
            except InterruptedError:
                if op != 'semTryWait':
                    raise
            except BlockingIOError as exc:
                if op == 'semTryWait' and exc.errno == _ipc_errno.EAGAIN:
                    return False
                raise
        if op == 'semClose':
            del _ipc_semaphores[handle]
        if op == 'semTryWait':
            return True
    return None


def _ipc_available(family):
    if family not in ('pipe', 'fifo', 'socketpair', 'poll', 'mq', 'shm', 'sem'):
        raise ValueError(f'unknown IPC mechanism {family!r}')
    if _ipc_os.name != 'posix':
        return False
    if family == 'pipe':
        return all(hasattr(_ipc_os, name) for name in ('pipe', 'set_blocking'))
    if family == 'fifo':
        return all(hasattr(_ipc_os, name) for name in ('mkfifo', 'O_NOFOLLOW'))
    try:
        if family == 'socketpair':
            import socket
            return hasattr(socket, 'socketpair')
        if family == 'poll':
            import select
            return hasattr(select, 'poll')
        if family == 'mq':
            _ipc_mq_api()
        if family == 'sem':
            _ipc_sem_api()
        if family == 'shm':
            if not all(hasattr(_ipc_os, name) for name in ('pread', 'pwrite', 'ftruncate')):
                return False
            _ipc_lib('shm', ('shm_open', 'shm_unlink'))
    except (LanguageError, ImportError):
        return False
    return True


def _ipc_write(fd, data):
    # An embedding host can enable SIGPIPE even though the Python CLI ignores it.
    # Block it on this thread only and preserve a SIGPIPE that was already pending.
    import signal
    signals = {signal.SIGPIPE}
    previous = signal.pthread_sigmask(signal.SIG_BLOCK, signals)
    try:
        pending = signal.sigpending()
        try:
            return _ipc_os.write(fd, data)
        except BrokenPipeError:
            if signal.SIGPIPE not in pending:
                if hasattr(signal, 'sigtimedwait'):
                    signal.sigtimedwait(signals, 0)
                elif signal.SIGPIPE in signal.sigpending():
                    signal.sigwait(signals)
            raise
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, previous)


def _ipc_execute(op, args):
    if op == 'available':
        return _ipc_available(args[0])
    if _ipc_os.name != 'posix':
        _ipc_unavailable('native IPC requires a POSIX host')
    if op == 'pipe':
        r, w = _ipc_os.pipe()
        return [r, w]
    if op == 'socketPair':
        import socket
        left, right = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
        return [left.detach(), right.detach()]
    if op in ('fifoCreate', 'fifoOpen', 'fifoUnlink'):
        path = args[0]
        if not path:
            raise ValueError('expected a nonempty FIFO path')
        if op == 'fifoCreate':
            _ipc_os.mkfifo(path, 0o600)
            return None
        if op == 'fifoUnlink':
            if not _ipc_stat.S_ISFIFO(_ipc_os.lstat(path).st_mode):
                raise ValueError('path is not a FIFO')
            _ipc_os.unlink(path)
            return None
        if not _ipc_stat.S_ISFIFO(_ipc_os.lstat(path).st_mode):
            raise ValueError('path is not a FIFO')
        flags = _ipc_os.O_WRONLY if args[1] else _ipc_os.O_RDONLY
        fd = _ipc_os.open(path, flags | _ipc_os.O_NONBLOCK | _ipc_os.O_NOFOLLOW)
        try:
            if not _ipc_stat.S_ISFIFO(_ipc_os.fstat(fd).st_mode):
                raise ValueError('path is not a FIFO')
        except BaseException:
            _ipc_os.close(fd)
            raise
        return fd
    if op.startswith('mq'):
        return _ipc_mq(op, args)
    if op.startswith('sem'):
        return _ipc_semaphore(op, args)
    if op in ('shmCreate', 'shmOpen', 'shmUnlink'):
        return _ipc_shm(op, args)
    if op.startswith('shm') and not _ipc_sys.platform.startswith('linux'):
        _ipc_unavailable('POSIX shared memory with positioned I/O requires a Linux host')
    fd = _ipc_number(args[0], 0, _IPC_INT_MAX, 'file descriptor')
    if op == 'close':
        _ipc_os.close(fd)
    elif op == 'setNonblocking':
        _ipc_os.set_blocking(fd, not args[1])
    elif op == 'read':
        return list(_ipc_os.read(fd, _ipc_number(args[1], 0, _IPC_LIMIT, 'read size')))
    elif op == 'write':
        return _ipc_write(fd, _ipc_bytes(args[1]))
    elif op in ('waitReadable', 'waitWritable'):
        import select
        wait = _ipc_number(args[1], 0, _IPC_INT_MAX, 'timeout in milliseconds')
        poller = select.poll()
        poller.register(fd, select.POLLIN if op == 'waitReadable' else select.POLLOUT)
        for _, events in poller.poll(wait):
            if events & select.POLLNVAL:
                raise OSError(_ipc_errno.EBADF, _ipc_os.strerror(_ipc_errno.EBADF))
            return True
        return False
    elif op in ('shmSize', 'shmRead', 'shmWrite'):
        st = _ipc_os.fstat(fd)
        if not _ipc_stat.S_ISREG(st.st_mode):
            raise ValueError('expected a shared memory descriptor')
        if op == 'shmSize':
            return st.st_size
        offset = _ipc_number(args[1], 0, (1 << 63) - 1, 'offset')
        if op == 'shmRead':
            return list(_ipc_os.pread(fd, _ipc_number(args[2], 0, _IPC_LIMIT, 'read size'), offset))
        data = _ipc_bytes(args[2])
        if offset > st.st_size or len(data) > st.st_size - offset:
            raise ValueError('write would extend the shared memory object')
        return _ipc_os.pwrite(fd, data, offset)
    else:
        raise ValueError('unknown IPC operation')
    return None


def ipc_call(definition, args):
    op = definition['symbol']
    try:
        params = definition['parameters']
        if len(args) != len(params):
            raise ValueError(f'expected {len(params)} arguments, got {len(args)}')
        for param, value in zip(params, args):
            kind = param['type']
            valid = ((kind == 'Int' and type(value) is int and -(1 << 63) <= value < (1 << 63)) or
                     (kind == 'Bool' and type(value) is bool) or
                     (kind == 'String' and type(value) is str and '\0' not in value) or
                     (kind == 'Vec<Int>' and type(value) is list))
            if not valid:
                raise ValueError(f'expected {kind} at the IPC boundary')
        return _ipc_execute(op, args)
    except LanguageError as exc:
        # _ipc_unavailable is the only LanguageError raised by these helpers.
        raise LanguageError('IPC_UNAVAILABLE', f'{op}: {exc.args[0]}') from None
    except (ImportError, AttributeError, NotImplementedError) as exc:
        raise LanguageError('IPC_UNAVAILABLE', f'{op}: {exc}') from None
    except OSError as exc:
        if exc.errno in (_ipc_errno.ENOSYS, _ipc_errno.ENOTSUP, _ipc_errno.EOPNOTSUPP):
            category = 'IPC_UNAVAILABLE'
        elif exc.errno in (_ipc_errno.EAGAIN, _ipc_errno.EWOULDBLOCK):
            category = 'IPC_WOULD_BLOCK'
        else:
            category = 'IPC'
        raise LanguageError(category, f'{op}: {exc.strerror} (errno {exc.errno})') from None
    except (ValueError, OverflowError) as exc:
        raise LanguageError('IPC', f'{op}: {exc}') from None
