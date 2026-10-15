(* Output is captured by the embedding API and flushed by the CLI. *)
let IO.print_int = print_int;;
let IO.println_int = println_int;;
let IO.print_string = print_string;;
let IO.print_endline = print_endline;;
let IO.print_bool b = print_string (Bool.to_string b);;
