;; dispatch.scm - benchmark for optimization #2 (primitive-interception
;; ladder in FRAME_CALL).
;;
;; The ladder (~15 sequential _is_X_primitive checks) runs only for calls
;; WITH arguments, which complete through FRAME_CALL.  Zero-arg calls take
;; a different, ladder-free path.  So:
;;
;;   call1 - 10 cheap ONE-arg calls per iteration  -> hits the ladder 10x
;;   call0 - 10 cheap ZERO-arg calls per iteration -> skips the ladder
;;
;; Both accumulate identically (each helper returns 1), so their checksums
;; MUST match - a fairness cross-check.  The quantity of interest is the
;; difference-in-differences across a before/after code change:
;;
;;   (call1_before - call1_after) - (call0_before - call0_after)
;;
;; which cancels everything except the ladder.  call0 should stay flat
;; after the change; if it moves, distrust the result.
;;
;; Portable across pyScheme and cppscheme2 (R7RS current-jiffy timing;
;; workloads are pure core Scheme).  Methodology identical to
;; env_lookup.scm: 11 runs, discard warm-up, headline = min, with
;; median/max/spread as the confidence check.

;; ---- knobs ------------------------------------------------------------
(define N      400000)   ;; loop iterations per run (tune for ~2-4 s) -- cppscheme2 (~18x faster)
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

;; ---- helpers under test ----------------------------------------------
(define (f0)  1)    ;; zero-arg  (ladder-free call path)
(define (f1 x) 1)   ;; one-arg   (FRAME_CALL ladder path)

;; ---- workloads --------------------------------------------------------
;; 10 zero-arg calls per iteration; the (= ...), (+ ...) and modulo are
;; shared overhead common to both workloads.
(define (call0 n)
  (let loop ((i 0) (acc 0))
    (if (= i n)
        acc
        (loop (+ i 1)
              (modulo (+ acc (f0)(f0)(f0)(f0)(f0)(f0)(f0)(f0)(f0)(f0))
                      1000000)))))

;; 10 one-arg calls per iteration; identical accumulation.
(define (call1 n)
  (let loop ((i 0) (acc 0))
    (if (= i n)
        acc
        (loop (+ i 1)
              (modulo (+ acc (f1 i)(f1 i)(f1 i)(f1 i)(f1 i)
                             (f1 i)(f1 i)(f1 i)(f1 i)(f1 i))
                      1000000)))))

;; ---- driver -----------------------------------------------------------
(define (bench label fn)
  (let ((chk (fn N)))                ;; correctness/fairness anchor
    (time-once (lambda () (fn N)))   ;; warm-up, discarded
    (report label (isort (collect RUNS (lambda () (fn N)))) chk)))

(display "dispatch benchmark (#2)  N=") (display N)
(display "  runs=") (display RUNS) (display " (+1 warm-up)") (newline)
(let ((c1 (bench "call1 (with-args, ladder)" call1))
      (c0 (bench "call0 (zero-arg, no ladder)" call0)))
  (display "call1 - call0 (min) = ") (display (secs (- c1 c0)))
  (display " s   <- ladder + per-arg-call overhead for 10 calls/iter")
  (newline))
