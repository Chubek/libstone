assert(arg[1] == "x86_64" or arg[1] == "aarch64" or arg[1] == "riscv64", "unsupported boot machine")
return "DomIOS ready: " .. arg[1]
