let Option.is_some x = match x with Some y -> true | None -> false;;
let Option.is_none x = not (Option.is_some x);;
let Option.value default x = match x with Some y -> y | None -> default;;
let Option.map f x = match x with Some y -> Some (f y) | None -> None;;
let Option.bind x f = match x with Some y -> f y | None -> None;;
let Option.get x = match x with Some y -> y | None -> failwith "Option.get: None";;
