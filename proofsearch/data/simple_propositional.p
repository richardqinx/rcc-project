% Simple propositional-style test
% P(a) and (P(X) → Q(X)) and ¬Q(a) should be unsatisfiable.
cnf(fact, axiom, p(a)).
cnf(rule, axiom, ~p(X) | q(X)).
cnf(goal, negated_conjecture, ~q(a)).
