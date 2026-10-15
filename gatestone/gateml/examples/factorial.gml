let rec factorial n = if n = 0 then 1 else n * factorial (n - 1);;
IO.println_int (factorial 5);
0
