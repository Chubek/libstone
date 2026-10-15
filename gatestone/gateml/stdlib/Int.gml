let Int.min_int = -9223372036854775808;;
let Int.max_int = 9223372036854775807;;
let Int.abs x = if x < 0 then -x else x;;
let Int.neg x = -x;;
let Int.compare x y = if x < y then -1 else if x > y then 1 else 0;;
let Int.clamp low high x = max low (min high x);;
let rec Int.gcd x y = if y = 0 then Int.abs x else Int.gcd y (x mod y);;
let Int.pow base exponent =
  let rec loop b n acc =
    if n = 0 then acc
    else loop (b * b) (n / 2) (if n mod 2 = 0 then acc else acc * b)
  in if exponent < 0 then failwith "negative exponent" else loop base exponent 1;;
