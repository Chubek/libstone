let Bool.not x = not x;;
let Bool.to_int x = if x then 1 else 0;;
let Bool.to_string x = if x then "true" else "false";;
let Bool.equal x y = if x then y else not y;;
