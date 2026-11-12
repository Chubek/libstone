(error
 (id "MS-OWN-0001")
 (name "use-after-move")
 (category memory-safety)
 (phase borrow-check)
 (severity error)
 (message "Value '{value}' is used after move")
 (pattern (use (moved ?value)))
 (repairs (repair "borrow-instead" (strategy synthgram)) (repair "clone-before" (strategy synthgram))))
