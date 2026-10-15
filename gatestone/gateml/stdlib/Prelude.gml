(* Strict, immutable, let-polymorphic functions. *)
let id x = x;;
let const x y = x;;
let flip f x y = f y x;;
let compose f g x = f (g x);;
let ignore x = ();;
let succ x = x + 1;;
let pred x = x - 1;;
let min x y = if x < y then x else y;;
let max x y = if x > y then x else y;;
