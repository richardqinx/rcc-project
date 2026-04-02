% ========================================================================= 
% Monkey and Banana Problem 
% State representation: state(Monkey, Box, Banana, Height, Situation) 
% ========================================================================= 

% 1. Initial state (corresponding sample input: A B C 0) 
cnf(init_state, axiom, state(a, b, c, h0, s0)). 

% 2. State transition axioms (action rules) 
% Action: Go(X) 
cnf(action_go, axiom, ~state(M, B, G, h0, S) | state(X, B, G, h0, do(go(X), S)) ). 

% Action: Push(X) (Note M and B use the same variable M representing M=B) 
cnf(action_push, axiom, ~state(M, M, G, h0, S) | state(X, X, G, h0, do(push(X), S)) ). 

% Action: Climb() 
cnf(action_climb, axiom, ~state(M, M, G, h0, S) | state(M, M, G, h1, do(climb, S)) ). 

% Action: Down() 
cnf(action_down, axiom, ~state(M, B, G, h1, S) | state(M, B, G, h0, do(down, S)) ). 

% Action: Reach() (Note M, B, G all use G uniformly, representing M=G and B=G) 
cnf(action_reach, axiom, ~state(G, G, G, h1, S) | goal_reached(do(reach, S)) ). 

% 3. Goal theorem (using proof by contradiction) 
% Goal: There exists a state S such that goal_reached(S) is true. 
cnf(negated_goal, negated_conjecture, ~goal_reached(S) ).