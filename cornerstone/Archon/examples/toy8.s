# Local labels are byte addresses. @start makes the second immediate PC-relative.
start: movi r1, end
       movi r2, @start
       swap r1, r2
end:   add r3, r1, r2
