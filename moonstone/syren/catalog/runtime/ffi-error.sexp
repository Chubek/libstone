(error (id "MS-FFI-0001") (name "foreign-call-failure") (category ffi)
 (phase runtime) (severity error) (message "Foreign call {name} failed")
 (pattern (ffi-error ?name)))
