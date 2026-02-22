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
    
    //parsing the input string into command structure
    parse_command(input, &cmd);
    
    //validating the parsed command
    if (!validate_command(&cmd)) 
    {
      //validation failed, error message already printed
      continue; //skipping to next iteration
    }
    
    //checking if command is a built-in
    if (is_builtin(cmd.command)) 
    {
      //executing built-in command in parent process
      execute_builtin(&cmd);
    } 
    else 
    {
      //executing external command in child process
      execute_command(&cmd);
    }
  }
  
  //exiting shell successfully
  exit(EXIT_SUCCESS);
}

