module Squares = struct
  let of_range low high = List.map (fun x -> x * x) (List.range low high);;
  let sum low high = List.sum (of_range low high);;
end;;
Squares.sum 1 10
