let List.is_empty xs = match xs with [] -> true | h :: t -> false;;
let List.length xs =
  let rec loop ys n = match ys with [] -> n | h :: t -> loop t (n + 1)
  in loop xs 0;;
let List.rev xs =
  let rec loop ys acc = match ys with [] -> acc | h :: t -> loop t (h :: acc)
  in loop xs [];;
let List.head xs = match xs with [] -> None | h :: t -> Some h;;
let List.tail xs = match xs with [] -> None | h :: t -> Some t;;
let rec List.map f xs = match xs with [] -> [] | h :: t -> f h :: List.map f t;;
let rec List.filter f xs = match xs with [] -> [] | h :: t ->
  if f h then h :: List.filter f t else List.filter f t;;
let rec List.fold_left f acc xs = match xs with [] -> acc | h :: t -> List.fold_left f (f acc h) t;;
let rec List.fold_right f xs acc = match xs with [] -> acc | h :: t -> f h (List.fold_right f t acc);;
let rec List.append xs ys = match xs with [] -> ys | h :: t -> h :: List.append t ys;;
let List.concat xs = List.fold_right List.append xs [];;
let List.concat_map f xs = List.concat (List.map f xs);;
let List.sum xs = List.fold_left (fun x y -> x + y) 0 xs;;
let List.product xs = List.fold_left (fun x y -> x * y) 1 xs;;
let rec List.exists f xs = match xs with [] -> false | h :: t -> f h || List.exists f t;;
let rec List.for_all f xs = match xs with [] -> true | h :: t -> f h && List.for_all f t;;
let List.mem x xs = List.exists (fun y -> x = y) xs;;
let rec List.find f xs = match xs with [] -> None | h :: t -> if f h then Some h else List.find f t;;
let rec List.nth xs n = match xs with [] -> None | h :: t ->
  if n < 0 then None else if n = 0 then Some h else List.nth t (n - 1);;
let rec List.take n xs = if n <= 0 then [] else match xs with [] -> [] | h :: t -> h :: List.take (n - 1) t;;
let rec List.drop n xs = if n <= 0 then xs else match xs with [] -> [] | h :: t -> List.drop (n - 1) t;;
let List.range low high =
  let rec loop n acc = if n < low then acc else if n = low then n :: acc else loop (n - 1) (n :: acc)
  in if low > high then [] else loop high [];;
let rec List.iter f xs = match xs with [] -> () | h :: t -> f h; List.iter f t;;
