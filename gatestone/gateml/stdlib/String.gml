(* Strings are byte sequences; indexes and lengths count bytes, not characters. *)
let String.is_empty s = String.length s = 0;;
let String.equal a b = String.compare a b = 0;;
let String.starts_with prefix s =
  let n = String.length prefix in
  n <= String.length s && String.sub s 0 n = prefix;;
let String.ends_with suffix s =
  let n = String.length suffix in
  let m = String.length s in n <= m && String.sub s (m - n) n = suffix;;
