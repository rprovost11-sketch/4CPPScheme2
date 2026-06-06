;; arith.scm - benchmark for optimization #8 (all-fixnum fast path in the
;; arithmetic primitives +, -, *).
;;
;; Every call to +, -, * currently runs _has_any_complex (TWO type
;; predicates per argument) and then routes each argument through _any_num
;; (another predicate + extractor) before computing.  For the common
;; all-integer case that complex scan is pure overhead; a fast path that
;; sums/multiplies the raw ints directly skips it.  This benchmark stresses
;; integer arithmetic so a before/after shows the fast path's win.
;;
;;   arith - a cluster of integer +,-,* ops per iteration  (hits the scan)
;;   ctrl  - the same loop with NO arithmetic cluster (just the loop's own
;;           (+ i 1) / (= i n) machinery), so it is barely touched by the
;;           fast path.  diff-in-diff cancels the shared loop machinery:
;;             (arith_before - arith_after) - (ctrl_before - ctrl_after)
;;           leaving the cluster's improvement.  ctrl should stay ~flat; if
;;           it moves a lot, distrust the result.
;;
;; Results are bounded with modulo so operands stay fixnum-sized -- bignum
;; growth would swamp the dispatch cost with Python bigint arithmetic and
;; hide what we are measuring.
;;
;; Portable across pyScheme and cppscheme2 (R7RS current-jiffy timing;
;; workloads are pure core Scheme).  Methodology identical to dispatch.scm:
;; 11 runs, discard warm-up, headline = min, with median/max/spread as the
;; confidence check.

;; ---- knobs ------------------------------------------------------------
(define N      60000)    ;; loop iterations per run (tune for ~2-4 s)
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
;; A cluster of integer +,-,* per iteration, accumulated mod 1e6 so the
;; operands stay small.  Roughly a dozen arithmetic primitive calls/iter.
(define (arith n)
  (let loop ((i 0) (acc 0))
    (if (= i n)
        acc
        (loop (+ i 1)
              (modulo (+ acc
                         (* i 3)
                         (* i 7)
                         (- i 1)
                         (+ i i)
                         (* 2 i)
                         (- (* i i) i))
                      1000000)))))

;; Same loop machinery, no arithmetic cluster: isolates the shared
;; (+ i 1) / (= i n) cost so diff-in-diff can subtract it out.
(define (ctrl n)
  (let loop ((i 0))
    (if (= i n)
        i
        (loop (+ i 1)))))

;; ---- driver -----------------------------------------------------------
(define (bench label fn)
  (let ((chk (fn N)))                ;; correctness anchor
    (time-once (lambda () (fn N)))   ;; warm-up, discarded
    (report label (isort (collect RUNS (lambda () (fn N)))) chk)))

(display "arith benchmark (#8)  N=") (display N)
(display "  runs=") (display RUNS) (display " (+1 warm-up)") (newline)
(let ((a (bench "arith (int +,-,* cluster)" arith))
      (c (bench "ctrl  (loop machinery only)" ctrl)))
  (display "arith - ctrl (min) = ") (display (secs (- a c)))
  (display " s   <- per-iter arithmetic cluster cost")
  (newline))
