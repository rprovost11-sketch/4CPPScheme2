(import (scheme base) (scheme write) (scheme char) (scheme read) (scheme inexact) (scheme complex) (scheme cxr) (scheme lazy) (scheme eval) (scheme repl) (scheme file) (scheme process-context) (scheme time) (scheme case-lambda) (scheme load))
(write (quasiquote (list (unquote (+ 1 2)) 4))) (newline)
