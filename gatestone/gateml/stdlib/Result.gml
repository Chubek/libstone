let Result.is_ok x = match x with Ok y -> true | Error e -> false;;
let Result.is_error x = not (Result.is_ok x);;
let Result.value default x = match x with Ok y -> y | Error e -> default;;
let Result.map f x = match x with Ok y -> Ok (f y) | Error e -> Error e;;
let Result.map_error f x = match x with Ok y -> Ok y | Error e -> Error (f e);;
let Result.bind x f = match x with Ok y -> f y | Error e -> Error e;;
let Result.to_option x = match x with Ok y -> Some y | Error e -> None;;
