// main.c
#include "fiskta.h"
#include <stdio.h>
#include <string.h>

enum Err parse_program(int, char**, Program*, const char**);
void parse_free(Program*);
enum Err engine_run(const Program*, const char*, FILE*);

static void die(enum Err e, const char *msg){
  if (msg) fprintf(stderr, "fiskta: %s\n", msg);
  else     fprintf(stderr, "fiskta: error %d\n", e);
}

int main(int argc, char **argv){
  if (argc < 3){ 
    fprintf(stderr,"Usage: fiskta <tokens...> <input-file|->\n"); 
    return 2; 
  }

  Program prg = {0}; 
  const char *path = NULL;
  enum Err e = parse_program(argc, argv, &prg, &path);
  if (e != E_OK){ 
    die(e, "parse error"); 
    parse_free(&prg); 
    return 2; 
  }

  e = engine_run(&prg, path, stdout);
  parse_free(&prg);
  if (e != E_OK){ 
    die(e, "execution error"); 
    return 2; 
  }
  return 0;
}
