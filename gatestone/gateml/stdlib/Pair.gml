let Pair.fst = fst;;
let Pair.snd = snd;;
let Pair.swap p = (snd p, fst p);;
let Pair.map f g p = (f (fst p), g (snd p));;
