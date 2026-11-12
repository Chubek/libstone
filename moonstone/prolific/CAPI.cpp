#include "Cache.hpp"
#include "prolific.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>

using namespace moonstone::prolific;
struct prolific_session {
    Recorder recorder;
    explicit prolific_session(Limits limits) : recorder(limits) {}
};
namespace {
thread_local char error_text[1024] = {};
void message(const char *text) { std::snprintf(error_text, sizeof(error_text), "%s", text); }
prolific_status status(Code code) {
    switch (code) {
    case Code::Ok:
        return PROLIFIC_OK;
    case Code::InvalidInput:
        message("invalid Prolific event or span order/thread");
        return PROLIFIC_INVALID_INPUT;
    case Code::Exhausted:
        message("Prolific recording limit exhausted");
        return PROLIFIC_EXHAUSTED;
    }
    return PROLIFIC_FAILURE;
}
template <class F> prolific_status call(F &&f) noexcept {
    error_text[0] = 0;
    try {
        return f();
    } catch (const std::invalid_argument &e) {
        message(e.what());
        return PROLIFIC_INVALID_INPUT;
    } catch (const std::exception &e) {
        message(e.what());
        return PROLIFIC_FAILURE;
    } catch (...) {
        message("unknown Prolific failure");
        return PROLIFIC_FAILURE;
    }
}
void check(prolific_session *session, bool args = true) {
    if (!session || !args) {
        if (session)
            session->recorder.record_loss();
        throw std::invalid_argument("null or invalid Prolific argument");
    }
}
Location location(const prolific_location *loc) {
    if (!loc)
        return {};
    return {loc->file ? loc->file : "", loc->ir_id ? loc->ir_id : "", loc->line, loc->column};
}
} // namespace

extern "C" {
prolific_status prolific_create(const prolific_config *config, prolific_session **out) {
    if (out)
        *out = nullptr;
    return call([&] {
        if (!out)
            throw std::invalid_argument("null Prolific output pointer");
        Limits limits;
        if (config) {
            if (config->abi_version != PROLIFIC_ABI_VERSION)
                throw std::invalid_argument("unsupported Prolific ABI version");
            limits = {config->max_events, config->max_bytes, config->max_text, config->max_threads,
                      config->max_depth};
        }
        *out = new prolific_session(limits);
        return PROLIFIC_OK;
    });
}
void prolific_destroy(prolific_session *session) { delete session; }
prolific_status prolific_begin(prolific_session *session, prolific_domain domain, const char *label,
                               const prolific_location *loc, uint64_t *span) {
    if (span)
        *span = 0;
    return call([&] {
        check(session, label && span && domain >= PROLIFIC_COMPILER && domain <= PROLIFIC_RUNTIME);
        auto result = session->recorder.begin(static_cast<Domain>(domain), label, location(loc));
        *span = result.span;
        return status(result.code);
    });
}
prolific_status prolific_end(prolific_session *session, uint64_t span) {
    return call([&] {
        check(session);
        return status(session->recorder.end(span));
    });
}
prolific_status prolific_record_loss(prolific_session *session) {
    return call([&] {
        check(session);
        session->recorder.record_loss();
        return PROLIFIC_OK;
    });
}
prolific_status prolific_emit(prolific_session *session, prolific_domain domain, prolific_kind kind,
                              const char *label, int64_t value, const char *detail,
                              const prolific_location *loc) {
    return call([&] {
        check(session, label && domain >= PROLIFIC_COMPILER && domain <= PROLIFIC_RUNTIME &&
                           kind >= PROLIFIC_INSTANT && kind <= PROLIFIC_FREE);
        return status(session->recorder.emit(static_cast<Domain>(domain), static_cast<Kind>(kind),
                                             label, value, detail ? detail : "", location(loc)));
    });
}
prolific_status prolific_report(prolific_session *session, char **json) {
    if (json)
        *json = nullptr;
    return call([&] {
        check(session, json != nullptr);
        Report report;
        report.data = session->recorder.snapshot();
        const auto text = report.json();
        auto *copy = static_cast<char *>(std::malloc(text.size() + 1));
        if (!copy)
            throw std::bad_alloc();
        std::memcpy(copy, text.c_str(), text.size() + 1);
        *json = copy;
        return PROLIFIC_OK;
    });
}
prolific_status prolific_save(prolific_session *session, const char *directory, const char *run,
                              int replace) {
    return call([&] {
        check(session, directory && run);
        Report report;
        report.data = session->recorder.snapshot();
        Cache cache(directory);
        cache.save(run, report, replace != 0);
        return PROLIFIC_OK;
    });
}
void prolific_free_string(char *text) { std::free(text); }
const char *prolific_last_error(void) { return error_text; }
} // extern C
