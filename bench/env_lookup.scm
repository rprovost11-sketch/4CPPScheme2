;; env_lookup.scm - environment-lookup microbenchmark.
;;
;; Portable across pyScheme and cppscheme2: self-times via R7RS
;; current-jiffy / jiffies-per-second and prints seconds, so it is
;; resolution-independent between the two interpreters.
;;
;; Two workloads do the IDENTICAL number of variable references and loop
;; iterations; only the lexical chain depth differs:
;;
;;   deep    - 12 nested lets; the inner loop reads all 12 outer locals
;;             plus the globals +,=,modulo from depth 12 every iteration,
;;             so each reference walks a long parent chain.
;;   shallow - the same 12 names bound in ONE frame; chain walks are
;;             trivial.  This is the CONTROL.
;;
;; (deep - shallow) isolates the chain-walk cost that optimization #5
;; (lexical addressing + global fast-path) targets.  If #5 speeds up
;; `deep` but barely moves `shallow`, the win is real and in the lookup
;; path.  modulo ... 1000000 keeps the accumulator a small fixnum so we
;; measure lookups, not bignum growth.
;;
;; Methodology: 11 runs per workload, discard the warm-up (run 1),
;; analyze the remaining 10.  Headline = min; median/max/spread are the
;; confidence check.  Trust the min only when min and median move
;; together and the spread is small.

;; ---- knobs ------------------------------------------------------------
(define N      50000)    ;; loop iterations per run (tune for ~2-5 s)
(define RUNS   10)       ;; analyzed runs (a warm-up run is added on top)

;; ---- timing -----------------------------------------------------------
(define (jps)    (exact->inexact (jiffies-per-second)))
(define (secs jf) (/ jf (jps)))

(define (time-once thunk)
  (let ((t0 (current-jiffy)))
    (thunk)
    (- (current-jiffy) t0)))

(define (collect n thunk)
  (let loop ((k n) (acc '()))
    (if (= k 0)
        acc
        (loop (- k 1) (cons (time-once thunk) acc)))))

;; ---- stats ------------------------------------------------------------
(define (insert x s)
  (cond ((null? s)        (list x))
        ((<= x (car s))   (cons x s))
        (else             (cons (car s) (insert x (cdr s))))))
(define (isort lst)
  (if (null? lst) '() (insert (car lst) (isort (cdr lst)))))

(define (median sorted)
  (let* ((n (length sorted)) (mid (quotient n 2)))
    (if (odd? n)
        (list-ref sorted mid)
        (/ (+ (list-ref sorted (- mid 1)) (list-ref sorted mid)) 2))))

(define (report label sorted chk)
  (let* ((lo  (car sorted))
         (hi  (list-ref sorted (- (length sorted) 1)))
         (med (median sorted))
         (spread (if (= lo 0) 0.0 (* 100.0 (/ (- hi lo) (exact->inexact lo))))))
    (display label) (display "  min=") (display (secs lo))
    (display " s  median=") (display (secs med))
    (display " s  max=") (display (secs hi))
    (display " s  spread=") (display spread) (display "%  chk=")
    (display chk) (newline)
    lo))

;; ---- workloads --------------------------------------------------------
(define (deep n)
  (let ((a 1)) (let ((b 2)) (let ((c 3)) (let ((d 4))
  (let ((e 5)) (let ((f 6)) (let ((g 7)) (let ((h 8))
  (let ((i 9)) (let ((j 10)) (let ((k 11)) (let ((l 12))
    (let loop ((x 0) (acc 0))
      (if (= x n)
          acc
          (loop (+ x 1)
                (modulo (+ acc a b c d e f g h i j k l) 1000000))))
  )))))))))))))

(define (shallow n)
  (let ((a 1)(b 2)(c 3)(d 4)(e 5)(f 6)(g 7)(h 8)(i 9)(j 10)(k 11)(l 12))
    (let loop ((x 0) (acc 0))
      (if (= x n)
          acc
          (loop (+ x 1)
                (modulo (+ acc a b c d e f g h i j k l) 1000000))))))

;; ---- driver -----------------------------------------------------------
(define (bench label fn)
  (let ((chk (fn N)))                ;; correctness anchor
    (time-once (lambda () (fn N)))   ;; warm-up, discarded
    (report label (isort (collect RUNS (lambda () (fn N)))) chk)))

(display "env-lookup benchmark  N=") (display N)
(display "  runs=") (display RUNS) (display " (+1 warm-up)") (newline)
(let ((d (bench "deep   " deep))
      (s (bench "shallow" shallow)))
  (display "deep - shallow (min) = ") (display (secs (- d s)))
  (display " s") (newline))
