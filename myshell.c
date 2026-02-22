#include "myshell.h"

int main(void) {
  char input[MAX_INPUT]; //buffer for storing user input
  Command cmd; //structure for storing parsed command
  
  //main shell loop - running infinitely until user exits
  while (1) 
  {
    //displaying the shell prompt
    printf("%s", PROMPT);
    fflush(stdout); //flushing output buffer to ensure prompt appears
    
    //reading user input from stdin
    if (fgets(input, MAX_INPUT, stdin) == NULL) 
    {
      //checking if Ctrl+D (EOF) was pressed
      printf("\n");
      break; //exiting shell loop
    }
    
    //checking if input was truncated (too long)
    size_t input_len = strlen(input);
    if (input_len > 0 && input[input_len - 1] != '\n' && input_len == MAX_INPUT - 1)
    {
      //input exceeded buffer size - clearing remaining input
      fprintf(stderr, "Error: Input too long (maximum %d characters).\n", MAX_INPUT - 2);
      //clearing remaining characters from input buffer
      int c;
      while ((c = getchar()) != '\n' && c != EOF);
      continue; //skipping to next iteration
    }
    
    //removing newline character at the end of input
    input[strcspn(input, "\n")] = '\0';
    
    //checking if user entered "exit" command
    if (strcmp(input, "exit") == 0) 
    {
      break; //exiting shell loop
    }
    
    //checking for empty input (user just pressed Enter)
    if (strlen(input) == 0) 
    {
      continue; // skipping to next iteration
    }
  if (strchr(input, '|') != NULL) {
  Pipeline p;
  parse_pipeline(input, &p);

  if (!validate_pipeline(&p)) {
    continue;
  }

  execute_pipeline(&p);
} else {
  // existing single-command behavior
  parse_command(input, &cmd);

  if (!validate_command(&cmd)) {
    continue;
  }

  if (is_builtin(cmd.command)) {
    execute_builtin(&cmd); // parent builtin (cd affects shell) — like real shells
  } else {
    execute_command(&cmd);
  }
}
  }
  
  //exiting shell successfully
  exit(EXIT_SUCCESS);
}

