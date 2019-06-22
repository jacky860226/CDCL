// CDCL.c
// Joshua Blinkhorn 29.04.2019
// This file is part of CDCL

// CURRENT UPGRADE: 1. literals as assignment pointers 2. pure literal elimination

// This upgrade will implement the trail as a ring buffer, and
// the decision heuristic as a linked list structure that holds
// the buffer. Reordering the links in the list allows to schedule
// locally relevent variables at the head of the buffer.
// Decision terminates when the ring buffer empties

#include "CDCL.h"
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include "getRSS.c"

#ifdef DEBUG
#define DEBUG_MSG(x) (x)
#else
#define DEBUG_MSG(x) 
#endif

// MACROS

// results
#define UNSAT 0
#define SAT 1

// truth values
#define POSITIVE 1
#define NEGATIVE -1

// highest decision level (indicates an avaliable decision)
#define DEC_MAX ULONG_MAX
#define MAX_VARS ULONG_MAX

// assignment types
#define DEC_ASS 0
#define PROP_ASS 1
#define CON_ASS 2

// assignment states -- ordered as they appear on the trail
#define DECEASED 1
#define ACTIVE 2
#define PENDING 3
#define AVAILABLE 4

// special values
#define NULL_DEC_LEVEL ULONG_MAX - 1
#define MAX_VARS ULONG_MAX / 2

// low level types
typedef unsigned char result_t;
typedef signed char truth_value_t;
typedef unsigned char ass_type_t;
typedef unsigned char ass_status_t;
typedef signed long int DIMACS_lit_t;
typedef unsigned long int cnf_size_t;
typedef unsigned long int model_size_t; 
typedef unsigned long int mutable_size_t;
typedef unsigned long int lit_t;
typedef signed long int DIMACS_lit_t;

// stats
clock_t start_time;

// higher level types 

// mutable (array)

// this is an automatically resized array of pointers to lit_t.
// it can be used to store a cnf, or a list of pointers to literals (e.g. the 
// watched literals for a singleton assignment), which can also be thought of
// as a set of unit clauses

// the array becomes twice as large whenever necessary to add an item

typedef struct ass {
  ass_status_t ass_status;
  truth_value_t truth_value;
  ass_type_t ass_type; 
  dec_level_t dec_level;
  cnf_size_t num_active;

  struct mutable {
    mutable_size_t size;
    mutable_size_t used;

    struct clause {
      struct ass** lits;
      model_size_t width;
    };
    
    struct clause** data;
  };

  struct mutable watched_lits;

} ass_t;

typedef struct mutable mutable_t;
typedef struct clause clause_t;

// assignment

// stores various information about a singleton assignment, including
// a list of watched literals


// clause

// cnf

// a CNF is a struct storing (1) an array of clauses and (2) the size of the array 

typedef struct cnf {
  clause_t* clauses;
  cnf_size_t size;
} cnf_t;

// trail

// a trail is an ordered sequence of pointers literals
// it is allocated as a fixed size array, since it size never exceeds
// the number of variables
// head and tail are pointers into the array used in propagation

typedef struct trail {
  ass_t** sequence;
  ass_t** head;
  ass_t** tail;
} trail_t;

// global solver

// data
dec_level_t dec_level = 0;
model_size_t num_vars;
model_size_t num_asses;

// stats variables
unsigned long num_conflicts = 0;
unsigned long num_decisions = 0;
unsigned long num_unit_props = 0;
unsigned long num_redefinitions = 0;

// decision pointers
ass_t* dec_head;
ass_t* dec_tail;

// organs
cnf_t cnf;
mutable_t learned_cnf;
ass_t* model;
trail_t trail;
state_t stat;

// IMPLEMENTATION

void error(char* message);

// LITERAL RELATED FUNCTIONS

ass_t* DIMACS_to_lit(DIMACS_lit_t value)
{
  // converts a DIMACS literal into an internal literal
  // internal literals are represented as values from 0 to (2 * num_vars) - 1
  // values from 0 to num_vars - 1 represent positive literals,
  // values from num_vars to (2 * num_vars) - 1 represent negative literals.
  return  model + ((value < 0 ? -1 : 1) * value) - 1 + ((value < 0) * num_vars);
}

DIMACS_lit_t lit_to_DIMACS(ass_t* lit)
{
  // converts an internal literal back into a DIMACS literal
  // this should only ever be used for printing literal values
  return (DIMACS_lit_t)(((lit - model) % num_vars) + 1) * (((lit - model) < num_vars) ? 1 : -1);
}

ass_t* get_comp_lit(ass_t* lit)
{
  // returns the complementary literal
  return lit + ((lit - model < num_vars? 1 : -1) * num_vars);
}

void lit_print(ass_t** lit)
{
  //(stderr, "%lu", *lit);
  // prints an internal literal in the form of a DIMACS literal
  fprintf(stderr, "%ld", lit_to_DIMACS(*lit));
}


// CLAUSE RELATED FUNCTIONS

clause_t clause_init(model_size_t width)
{
  // initialises a clause of size `width'
  // the fist `literal' is the size of the clause 
  clause_t clause;

  clause.width = width;
  if ((clause.lits = (ass_t**)malloc(sizeof(ass_t*) * (width))) == NULL)
    error("cannot allocate clause literals");

  return clause;
}

char clause_is_unit(clause_t* clause)
{
  // returns 1 if the given clause is a unit clause, 0 otherwise
  return (clause->width == 1) ? 1 : 0;
}

void clause_free(clause_t* clause)
{
  free(clause->lits);
}

void clause_print(clause_t* clause)
{
  model_size_t num_lits = clause->width;
  model_size_t which_lit;

  for(which_lit = 0; which_lit < num_lits; which_lit++)
    {
      lit_print(clause->lits + which_lit);
      fprintf(stderr, " ");
    }
  fprintf(stderr, " 0\n");
}

// CNF RELATED FUNCTIONS

void cnf_print()
{
  cnf_size_t which_clause;
  
  fprintf(stderr, "FORMULA:\n");
  for(which_clause = 0; which_clause < cnf.size; which_clause++)
    {
      clause_print(cnf.clauses + which_clause);
    }
}


// MUTABLE RELATED FUNCTIONS

void mutable_init(mutable_t* mutable)
{
  // allocate memory for data - default size is 1
  if ((mutable->data = (clause_t**)malloc(sizeof(clause_t*))) == NULL)
    error("cannot allocate mutable data");

  // set default member values
  mutable->size = 1L;
  mutable->used = 0L;
}

// use this to free a watched literal list
void mutable_free(mutable_t* mutable)
{
  // frees only the data of the pointed to mutable
  free(mutable->data);
}

// use this to free learned cnf
void mutable_free_clauses(mutable_t* mutable)
{
  // frees both the data of the pointed to mutable, and the clauses
  // that the data points to
  cnf_size_t which_clause, num_clauses;

  num_clauses = mutable->used;
  for(which_clause = 0; which_clause < num_clauses; which_clause++)
    clause_free((clause_t*)*(mutable->data + which_clause));
  free(mutable->data);
}

void mutable_push(mutable_t* mutable, clause_t* datum)
{
  // pushes the datum onto the mutable's data array
  mutable_size_t size;

  if (mutable->used == (size = mutable->size))
    {
      if ((mutable->data = 
	   (clause_t**)realloc(mutable->data, 2 * size * sizeof(clause_t*))) == NULL)
	error("cannot reallocate mutable data");
      mutable->size = 2 * size;
      mutable->data[mutable->used++] = datum;
    }
  else
    {
      mutable->data[mutable->used++] = datum;      
    }
}

// use to print a learned cnf
void mutable_print_clauses(mutable_t* mutable)
{
  mutable_size_t which_clause, num_clauses;
  clause_t** data;
  
  fprintf(stderr, "LEARNED CLAUSES:\n");
  fprintf(stderr, "size: %lu, used: %lu\n", mutable->size, mutable->used);

  num_clauses = mutable->used;
  data = mutable->data;
  for(which_clause = 0; which_clause < num_clauses; which_clause++)
    {
      clause_print(data[which_clause]);
      fprintf(stderr, "\n");
    }
}


/*
// use to print a watched literal list // TODO deprecate, or hone for debugginf
void mutable_print_lits(mutable_t* mutable)
{
  mutable_size_t which_lit, num_lits;
  lit_t** data;
  
  fprintf(stderr, "size: %lu, used: %lu, literals:", mutable->size, mutable->used);

  num_lits = mutable->used;
  data = mutable->data;
  for(which_lit = 0; which_lit < num_lits; which_lit++)
    {
      lit_print(data + which_lit);
    }
  fprintf(stderr, "\n");
}

void print_watched_lits() // TODO deprecate, or hone for debugging
{
  model_size_t which_ass;

  fprintf(stderr, "WATCHED LITERALS\n");
  for (which_ass = 0; which_ass < num_asses; which_ass++)
    {
      mutable_print_lits(&(model[which_ass].watched_lits));
    }
  fprintf(stderr, "\n");
}

*/

// ASSIGNMENT RELATED FUNCTIONS

void assign(ass_t* lit)
{
  // overwrites the assignment satisfying the literal into the model
  // the assignment type should already have been set when the literal
  // was added to the trail
  ass_t* comp_lit = get_comp_lit(lit);

  lit->truth_value = POSITIVE;
  lit->dec_level = dec_level;
  comp_lit->truth_value = NEGATIVE;
  comp_lit->dec_level = dec_level;
}

void unassign(ass_t* lit)
{
  // unassigns the variable for this literal (i.e. for the literal and its complement)
  lit->dec_level = DEC_MAX;
  get_comp_lit(lit)->dec_level = DEC_MAX;
}

// prints the value of an assignment and its decision level
void ass_print(ass_t* ass)
{
  if(ass->dec_level == DEC_MAX)
    fprintf(stderr, "0");
  else
    {
      fprintf(stderr, "%2d / %lu ", ass->truth_value, ass->dec_level);
      
      /* TODO: delete if ass_type is deprecated
      switch(ass->ass_type)
	{
	case DEC_ASS:
	  fprintf(stderr, "D ");
	  break;
	case PROP_ASS:
	  fprintf(stderr, "P ");
	  break;
	case CON_ASS:
	  fprintf(stderr, "C ");
	  break;
	default:
	  break;
	}
      */
    }
}

// this function assumes trail.head > trail.sequence
void backtrack(dec_level_t new_dec_level)
{  
  DEBUG_MSG(fprintf(stderr,
		    "In backtrack(). Backtracking to decision level %lu.\n",
		    new_dec_level));
  // go backwards through the trail and delete the assignments
  while((trail.head >= trail.sequence) && ((*(trail.head))->dec_level > new_dec_level))
    {
      unassign(*trail.head);
      trail.head--;
    }
  trail.head++;
  trail.tail = trail.head;
  //revert to given decision level
  dec_level = new_dec_level;
}

// MODEL RELATED FUNCTIONS

// prints the contents of the given model
void print_model()
{
  model_size_t which_var;

  fprintf(stderr, "MODEL:\n");      
    
  // print the assignment
  for(which_var = 0; which_var < num_vars; which_var++)
    {
      fprintf(stderr, "%lu: ", which_var + 1);
      ass_print(model + which_var);
      fprintf(stderr, "\n");
    }
  fprintf(stderr, "\n");
}
 
// TRAIL RELATED FUNCTIONS

void trail_queue_lit(ass_t* lit, ass_type_t ass_type)
{
  ass_t* comp_lit = get_comp_lit(lit);

  // set the assignment ass pending
  lit->ass_type = ass_type;
  lit->ass_status = PENDING;
  comp_lit->ass_type = ass_type;
  comp_lit->ass_status = PENDING;

  // place the assignment at the tail of the trail
  *(trail.tail) = lit;
  trail.tail++;
}

void print_trail()
{
  ass_t** temp_pointer;

  fprintf(stderr, "TRAIL: ");

  if (trail.sequence == trail.tail) 
    {
      fprintf(stderr, " (empty)\n\n");
      return;
    }
  fprintf(stderr, "\n");
  for(temp_pointer = trail.sequence; temp_pointer < trail.tail; temp_pointer++)
    {
      fprintf(stderr, "%lu: %ld ",
	      temp_pointer - trail.sequence + 1,
	      lit_to_DIMACS(*temp_pointer));
      if ((*temp_pointer)->ass_status == PENDING)
	fprintf(stderr, "W ");
      else switch((*temp_pointer)->ass_type)
	     {
	     case DEC_ASS:
	       fprintf(stderr, "D ");
	       break;
	     case PROP_ASS:
	       fprintf(stderr, "P ");
	       break;	
	     case CON_ASS:
	       fprintf(stderr, "C ");
	       break;	
	     default:
	       break;
	     }
      if (temp_pointer == trail.head) fprintf(stderr, "HEAD");
      if (temp_pointer == trail.tail) fprintf(stderr, "TAIL");
      fprintf(stderr, "\n");
    }
  fprintf(stderr, "\n");
}

// CDCL INTERFACE IMPLEMENTATION

void  CDCL_print_stats()
{
  fprintf(stderr, "Conflicts:         %lu\n", num_conflicts);
  fprintf(stderr, "Decisions:         %lu\n", num_decisions);
  fprintf(stderr, "Unit Propagations: %lu\n", num_unit_props);
  //fprintf(stderr, "Redefinitions:     %lu\n", num_redefinitions);
  fprintf(stderr, "%1.1lfs ", ((double)(clock() - start_time)) / CLOCKS_PER_SEC);
  fprintf(stderr, "%1.1zdMb ", getPeakRSS() / 1048576);
  fprintf(stderr, "\n");
}

void CDCL_report_SAT()
{
  //print_model();
  fprintf(stderr, "v SAT\n");
  CDCL_print_stats();
  exit(0);
}
void CDCL_report_UNSAT()
{
  fprintf(stderr, "v UNSAT\n");
  CDCL_print_stats();
  exit(0);
}

// initialises global solver according to the DIMACS file 'input'.
void CDCL_init(char* DIMACS_filename)
{
  FILE* input, *cursor;
  char buffer[5]; // used only to read the `cnf' string from the input file
  char ch; // used to read single characters from the input file  
  DIMACS_lit_t DIMACS_lit; // temporary literal for reading
  model_size_t width; 
  model_size_t which_lit; 
  model_size_t which_ass; 
  cnf_size_t which_clause;
  clause_t clause;
  ass_t* lit;

  start_time = clock();
  state = DECIDE;

  // open file connections
  // TODO: currently using two file connections to find size of clauses before writing
  // them; it is probably possible to use just one, and to traverse the stream 
  // backwards when needed
  if ((input = (FILE *)fopen(DIMACS_filename, "r")) == NULL) error("cannot open file");
  if ((cursor = (FILE *)fopen(DIMACS_filename, "r")) == NULL) error("cannot open file");

  // parse header
  // disregard comment lines
  while ((ch = fgetc(input)) == 'c')
    while ((ch = fgetc(input)) != '\n')
      continue;
  while ((ch = fgetc(cursor)) == 'c')
    while ((ch = fgetc(cursor)) != '\n')
      continue;
  // read header line
  if (ch != 'p') error("bad input - 'p' not found");
  if ((fscanf(input, "%s", buffer)) != 1) error("bad input - 'cnf' not found");
  fscanf(cursor, "%s", buffer);

  // read number of variables
  if ((fscanf(input, "%lu", &(num_vars))) != 1)
    error("bad input - number of vars missing");
  fscanf(cursor, "%lu", &(num_vars));

  if (num_vars > pow(2,(sizeof(lit_t) * 8) - 3) - 1) // i.e. more variables than our data type can handle
    error("too many vars");
  num_asses = num_vars * 2;

  // initialise model
  if ((model = (ass_t*)malloc(sizeof(ass_t) * num_asses)) == NULL)
	error("cannot allocate model");
  for (which_ass = 0; which_ass < num_asses; which_ass++)
    {
      // set default values and initialise watched literals mutable for each assignment
      model[which_ass].ass_status = AVAILABLE; 
      model[which_ass].num_active = 0;
      mutable_init(&(model[which_ass].watched_lits));
    }

  // initialise trail
  if ((trail.sequence = (ass_t**)malloc(sizeof(ass_t*) * num_asses)) == NULL)
	error("cannot allocate trail sequence");
  trail.head = trail.sequence;
  trail.tail = trail.sequence;

  // read number of clauses
  if(fscanf(input, "%lu", &cnf.size) != 1)
    error("bad input - number of clauses not found");
  fscanf(cursor, "%lu", &cnf.size);

  if (cnf.size == 0)
    {
      CDCL_report_SAT();
      exit(0);
    }
  if (num_vars == 0)
    {
      CDCL_report_UNSAT();
      exit(0);
    }

  // initialise cnf
  if ((cnf.clauses = (clause_t*)malloc(sizeof(clause_t) * cnf.size)) == NULL)
	error("cannot allocate cnf clauses");

  // allocate clauses and add to formula
  for(which_clause = 0; which_clause < cnf.size; which_clause++)
    {
      // find width of clause with cursor
      fscanf(cursor, "%ld", &DIMACS_lit);
      for(width = 0; DIMACS_lit != 0; width++) 
	fscanf(cursor, "%ld", &DIMACS_lit);

      // if the width is 0, note that the formula is UNSAT and return
      if (width == 0)
	{
	  CDCL_report_UNSAT();
	  exit(0);
	}

      // if the width is 1, add the assignment as a level 0 unit propagation
      if (width == 1)
	{
	  fscanf(input, "%ld", &DIMACS_lit);
	  trail_queue_lit(DIMACS_to_lit(DIMACS_lit), PROP_ASS);
	  fscanf(input, "%ld", &DIMACS_lit);
	  state = PROPAGATE;
	  // we do not store unit clauses, so decrement counters
	  cnf.size--;
	  which_clause--;
	}
      else 
	{
	  // initialise clause
	  clause = clause_init(width);
	  // set literals with input
	  which_lit = 0;
	  fscanf(input, "%ld", &DIMACS_lit);
	  while(DIMACS_lit != 0) 
	    {
	      clause.lits[which_lit] = (lit = DIMACS_to_lit(DIMACS_lit));
	      lit->num_active++;
	      fscanf(input, "%ld", &DIMACS_lit);
	      which_lit++;
	    }
	  
	  // put the clause into the cnf
	  cnf.clauses[which_clause] = clause;
	  
       	  // watch the first two literals
	  mutable_push(&(get_comp_lit(clause.lits[0])->watched_lits),
		       cnf.clauses + which_clause);
	  mutable_push(&(get_comp_lit(clause.lits[1])->watched_lits),
		       cnf.clauses + which_clause);
	}
    }
  
  // set default decision level
  dec_level = 0;
  // initialise empty learned clause list
  mutable_init(&(learned_cnf));
}

void CDCL_free()
{
  cnf_size_t which_clause;
  model_size_t which_ass;

  // free memory for the cnf
  for (which_clause = 0; which_clause < cnf.size; which_clause++)
    clause_free(cnf.clauses + which_clause);
  free(cnf.clauses);
    
  // free memory for the learned cnf
  mutable_free_clauses(&learned_cnf);

  // free memory for the model
  for (which_ass = 0; which_ass < num_asses; which_ass++)
    mutable_free(&(model[which_ass].watched_lits));
  free(model);
  
  // free memory for the trail
  free(trail.sequence);
}

// TODO: the watched literals should be the first two in the clause
// then we can deal with them by swapping them explicitly with some other literal,
// if possible

// handles the entire propagation cycle in a single function

// head should immediately precede tail of the trail when this function
// is called,
// in case of conflict, tail will exceed head when it returns 
// otherwise tail and head will be identical

// this function is responsible for all watched literals handling, except for the
// assignment of inital watched literals of learned clause, which is handled
// by conflict analysis

// the assignment at head when the function is called is handled first
// the watched literals are visited in order
// the function first attemps to push the watch to another literal
// if the clause is found to be unit, the assignment is checked for conflict
// if conflict is found, the watched literals for the current assignment are
// cleaned up and the function returns CONFLICT
// if no conflict is found, the unit assignment is added to the tail of the trail
// when all clauses have been visited, the trail head is incremented
// the function returns NO_CONFLICT when head and tail are again identical

state_t CDCL_prop()
{

  ass_t* temp_lit;
  model_size_t which_lit;
  ass_t** lits;
  cnf_size_t num_clauses, which_clause;
  mutable_t new_watchers;
  ass_t* propagator;
  clause_t** data;
  clause_t* clause;
  char clause_is_unit, clause_is_extinct;
  //ass_t* ass;

  DEBUG_MSG(fprintf(stderr, "In CDCL_prop()..\n"));
  
  while (trail.head != trail.tail)
    {
      // store the literal that we are propagating
      propagator = *(trail.head);
      
      // add the propagating assignment to the model, set all successive additions
      // as propagations
      assign(propagator);
      
      // make a local pointer to the list of watched literals, and get the list size 
      data = propagator->watched_lits.data;
      num_clauses = propagator->watched_lits.used;

      // initialise a new mutable for the replacement list
      mutable_init(&new_watchers);

      DEBUG_MSG(fprintf(stderr, "Propagating literal %ld on %lu clauses\n",
			lit_to_DIMACS(propagator), num_clauses));
      
      // cycle through the watched literals' clauses
      for (which_clause = 0; which_clause < num_clauses; which_clause++)
	{
	  // get the literals and the clause width
	  clause = data[which_clause];

	  if(clause != NULL)
	    {
	      lits = clause->lits;
	      clause_is_unit = 0;
	      clause_is_extinct = 0;
	      
	      // place the watched literal at the front of the clause
	      if(lits[0] != propagator)
		{
		  temp_lit = lits[0];
		  lits[0] = lits[1];
		  lits[1] = temp_lit;		  
		}
	      
	      DEBUG_MSG(fprintf(stderr, "Dealing with clause: "));
	 	      
	      // if the other watched literal is deceased, it must also
	      // be assigned positively -- so the clause can be deleted
	      if (lits[1]->dec_level == 0)
		{
		  // the clause is extinct
		  // delete the clause, and decrement active literal counters
		  data[which_clause] = NULL;
		  for (which_lit = 0; which_lit < clause->width; which_lit++)
		    lits[which_lit]->num_active--;

		  DEBUG_MSG(fprintf(stderr, " -> NULL"));
		  DEBUG_MSG(fprintf(stderr, 
				    " (other watched literal is deceased)\n"));
		  break;
		}
	      
	      // if the other watched literal is active, it must be satisfied
	      if (lits[1]->ass_status == ACTIVE)
		{
		  // so the watched literal is preserved
		  mutable_push(&new_watchers, clause);
		  
		  DEBUG_MSG(fprintf(stderr, " -> "));
		  DEBUG_MSG(clause_print(clause));
		  DEBUG_MSG(fprintf(stderr, 
				    " (no change - other watched literal is satisfied)\n"));
		  break;
		}

	      // cycle through the candidate literals and attempt a swap
	      clause_is_unit = 1; // tentative assignment, set to 0 when needed
	      
	      for(which_lit = 0; which_lit < clause->width - 2; which_lit++)
		{
		  if (lits[which_lit]->ass_status == DECEASED &&
		      lits[which_lit]->truth_value == POSITIVE)
		    {
		      // the clause is extinct
		      // delete the clause, and decrement active literal counters
		      clause_is_extinct = 1;
		      data[which_clause] = NULL;
		      for (which_lit = 0; which_lit < (clause->width); which_lit++)
			lits[which_lit]->num_active--;
		      
		      DEBUG_MSG(fprintf(stderr, " -> NULL"));
		      DEBUG_MSG(fprintf(stderr, 
					" (a candidate literal is deceased and positive)\n"));
		      
		      break;
		    }
		  
		  if (lits[which_lit]->ass_status == PENDING ||
		      lits[which_lit]->ass_status == AVAILABLE ||
		      (lits[which_lit]->ass_status == ACTIVE &&
		       lits[which_lit]->truth_value == POSITIVE))
		    {
		      // we found an eligible literal
		      // swap it for the original watched literal
		      clause_is_unit = 0;
		      temp_lit = lits[0];
		      lits[0] = lits[which_lit];
		      lits[which_lit] = temp_lit;
		      
		      // add it to the appropriate list
		      mutable_push(&(get_comp_lit(lits[0])->watched_lits),
				   clause);
		      
		      DEBUG_MSG(fprintf(stderr, " -> "));
		      DEBUG_MSG(clause_print(clause));
		      DEBUG_MSG(fprintf(stderr, "\n"));
		      
		      // force inner loop to terminate
		      break;
		    }
		}
	      
	      if (clause_is_unit == 1 && clause_is_extinct == 0)
		{
		  // we have a unit clause based on the other watched literal
		  DEBUG_MSG(fprintf(stderr, "found unit clause %ld",
				    lit_to_DIMACS(lits[1])));
		  num_unit_props++; // TODO: increment here or when processed?
		  // the watched literal should be placed on the replacement list
		  mutable_push(&new_watchers, clause);  
		  
		  // check if the negation of the unit literal is on the trail
		  if ((lits[1])->ass_status == AVAILABLE)
		    {
		      trail_queue_lit(lits[1], PROP_ASS);
		      DEBUG_MSG(fprintf(stderr,
					" -- added to trail.\n"));
		    }
		  else
		    {
		      // the status of lits[1] is PENDING
		      if ((lits[1])->truth_value == NEGATIVE)
			{
			  // the implied assignment yields a conflict
			  DEBUG_MSG(fprintf(stderr,
					    " -- detected conflict - aborting propagation.\n"));
			  // add clauses for unprocessed watched literals to replacement
			  // list
			  for (which_clause++; which_clause < num_clauses; which_clause++)
			    mutable_push(&new_watchers, clause);
			  // free the old data and instate the new list
			  mutable_free(&(propagator->watched_lits));
			  propagator->watched_lits = new_watchers;
			  return CONFLICT;
			}
		    }
		}
	      
	    }
	}
      // propagation for this assignment has completed without conflict
      // instate new watched lits      
      mutable_free(&(propagator->watched_lits));
      propagator->watched_lits = new_watchers;
      
      // increment head
      trail.head++;
      DEBUG_MSG(fprintf(stderr,
			"Completed propagation on literal %ld without conflict\n",
			lit_to_DIMACS(propagator)));
    }
  
  // propagation terminates without a conflict
  DEBUG_MSG(fprintf(stderr,"Propagation cycle complete.\n"));
  return DECIDE;
}
		  
// easy decision heuristic: assigns the first unassigned variable positively 
// trail.head and trail.tail are always equal when CDCL_decide() is called
// and point to the location after the final assignment
// if the function returns PROPAGATE, trail.head points to the location of
// the final assignment, trail.tail to the next location.  

char CDCL_decide()
{
  model_size_t which_var;

  DEBUG_MSG(fprintf(stderr, "In CDCL_decide(). "));

  if (trail.head == trail.sequence + num_vars)
    return SUCCESS;

  // find an unassigned var
  for (which_var = 0; which_var < num_vars; which_var++)
    {
      if(model[which_var].ass_status == AVAILABLE)
	{
	  // TODO : delete when bug free
	  model[which_var].ass_type = DEC_ASS; 
	  model[which_var + num_vars].ass_type = DEC_ASS;

	  // put the assignment on the trail (here the assignment is always positive)
	  trail_queue_lit(model + which_var, DEC_ASS);
	  // update decision level
	  dec_level++;
	  num_decisions++;

	  DEBUG_MSG(fprintf(stderr, "Made decision %lu.\n",
			    lit_to_DIMACS(model + which_var)));
	  DEBUG_MSG(print_model());
	  DEBUG_MSG(print_trail());
	  
	  return PROPAGATE;
	}	
    }
  DEBUG_MSG(fprintf(stderr, "No decision possible.\n"));
  return SUCCESS;
}


// the simplest clause learning: DPLL-style
// learn clauses that encode DPLL brandhing
void CDCL_repair_conflict()
{
  clause_t learned_clause;
  model_size_t which_var;

  DEBUG_MSG(fprintf(stderr, "In CDCL_repair_conflict."));
  
  num_conflicts++;
  if (dec_level == 0) CDCL_report_UNSAT();

  // decision level 1 is a special case - no clause actually need be learned
  if (dec_level != 1)
    {
      // construct the learned clause
      learned_clause = clause_init(dec_level);
      for (which_var = 0; which_var < num_vars; which_var++)
	{
	  if((model[which_var].ass_status != AVAILABLE) &&
	     (model[which_var].ass_type == DEC_ASS))
	    {
	      // put the highest decision level literals first
	      learned_clause.lits[dec_level - model[which_var].dec_level] = 
		(model[which_var].truth_value == POSITIVE) ? 
		get_comp_lit(model + which_var) : model + which_var;
	    }
	}
      DEBUG_MSG(fprintf(stderr, "Learned clause: "));
      DEBUG_MSG(clause_print(&learned_clause));
      DEBUG_MSG(fprintf(stderr, "\n"));
      // watch the highest level literals in the clause
      mutable_push(&(get_comp_lit(learned_clause.lits[0])->watched_lits), &learned_clause);
      mutable_push(&(get_comp_lit(learned_clause.lits[1])->watched_lits), &learned_clause);
      // add clause to the formula
      mutable_push(&learned_cnf, &learned_clause);
    }
  // note that backtracking leaves the head pointing to the last decision assignment.
  // backtrack, and add the negation of the last decision (as PROP_ASS) to the trail
  backtrack(dec_level - 1);
  trail_queue_lit(get_comp_lit(*(trail.head)), CON_ASS);
}

void CDCL_print()
{
  cnf_print();
  fprintf(stderr, "\n");
  mutable_print_clauses(&learned_cnf);
  print_model();
  print_trail();
}

//  error function implementation
void error(char* message)
{
  fprintf(stderr, "FATAL ERROR: %s.\n", message);
  exit(1);
}
