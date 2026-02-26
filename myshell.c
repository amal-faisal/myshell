#include "myshell.h"

int main(void) 
{
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
    
    //checking for empty input (user just pressed enter)
    if (strlen(input) == 0) 
    {
      continue; //skipping to next iteration
    }
    
    //checking if input contains pipe operator
    if (strchr(input, '|') != NULL) 
    {
      Pipeline p;
      parse_pipeline(input, &p);

      if (!validate_pipeline(&p)) 
      {
        continue;
      }

      execute_pipeline(&p);
    } 
    else 
    {
      //handling single-command execution
      parse_command(input, &cmd);

      if (!validate_command(&cmd)) 
      {
        continue;
      }

      if (is_builtin(cmd.command)) 
      {
        //executing builtin with redirection support
        //saving original file descriptors
        int saved_stdin = -1, saved_stdout = -1, saved_stderr = -1;
        int redirect_success = 1;
        
        //handling input redirection
        if (cmd.input_file != NULL) 
        {
          saved_stdin = dup(STDIN_FILENO);
          int fd_in = open(cmd.input_file, O_RDONLY);
          if (fd_in == -1) 
          {
            perror(cmd.input_file);
            redirect_success = 0;
          } 
          else 
          {
            dup2(fd_in, STDIN_FILENO);
            close(fd_in);
          }
        }
        
        //handling output redirection
        if (redirect_success && cmd.output_file != NULL) 
        {
          saved_stdout = dup(STDOUT_FILENO);
          int fd_out = open(cmd.output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fd_out == -1) 
          {
            perror(cmd.output_file);
            redirect_success = 0;
          } 
          else 
          {
            dup2(fd_out, STDOUT_FILENO);
            close(fd_out);
          }
        }
        
        //handling error redirection
        if (redirect_success && cmd.error_file != NULL) 
        {
          saved_stderr = dup(STDERR_FILENO);
          int fd_err = open(cmd.error_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fd_err == -1) 
          {
            perror(cmd.error_file);
            redirect_success = 0;
          } 
          else 
          {
            dup2(fd_err, STDERR_FILENO);
            close(fd_err);
          }
        }
        
        //executing builtin if redirections succeeded
        if (redirect_success) 
        {
          execute_builtin(&cmd);
        }
        
        //restoring original file descriptors
        if (saved_stdin != -1) 
        {
          dup2(saved_stdin, STDIN_FILENO);
          close(saved_stdin);
        }
        if (saved_stdout != -1) 
        {
          dup2(saved_stdout, STDOUT_FILENO);
          close(saved_stdout);
        }
        if (saved_stderr != -1) 
        {
          dup2(saved_stderr, STDERR_FILENO);
          close(saved_stderr);
        }
      } 
      else 
      {
        execute_command(&cmd);
      }
    }
  }
  
  //exiting shell successfully
  exit(EXIT_SUCCESS);
}

