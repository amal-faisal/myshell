#include "myshell.h"

//function for validating parsed command
//returns 1 if valid, 0 if invalid
int validate_command(Command *cmd) 
{
  
  //checking if parsing had errors
  if (cmd->has_error) 
  {
    fprintf(stderr, "Error: %s\n", cmd->error_msg);
    return 0;
  }
  
  //checking if command is empty
  if (cmd->command == NULL) 
  {
    fprintf(stderr, "Error: No command specified.\n");
    return 0;
  }
  
  //checking if input file exists (for input redirection)
  if (cmd->input_file != NULL) 
  {
    //attempting to open file for reading to check if it exists
    int fd = open(cmd->input_file, O_RDONLY);
    if (fd == -1) 
    {
      //file does not exist or cannot be opened
      perror(cmd->input_file);
      return 0;
    }

    //closing file descriptor after checking
    close(fd);
  }
  
  //command is valid
  return 1;
}

//function for executing command in a child process
void execute_command(Command *cmd) 
{
  pid_t pid; //process id
  int status; //exit status of child process
  
  //forking process to create child
  switch (pid = fork()) 
  {
    case -1:
      //fork failed - printing error
      perror("fork failed");
      return;
      
    case 0:
      //child process - executing command
      
      //handling input redirection
      if (cmd->input_file != NULL) 
      {
        //opening input file for reading
        int fd_in = open(cmd->input_file, O_RDONLY);
        if (fd_in == -1) 
        {
          //file cannot be opened - printing error
          perror(cmd->input_file);
          _exit(1);
        }

        //redirecting stdin (fd 0) to the input file
        if (dup2(fd_in, STDIN_FILENO) == -1) 
        {
          perror("dup2 input");
          close(fd_in);
          _exit(1);
        }

        //closing original file descriptor after duplication
        close(fd_in);
      }
      
      //handling output redirection
      if (cmd->output_file != NULL) 
      {
        //opening output file for writing (creating if needed, truncating if exists)
        int fd_out = open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out == -1) 
        {
          //file cannot be opened - printing error
          perror(cmd->output_file);
          _exit(1);
        }

        //redirecting stdout (fd 1) to the output file
        if (dup2(fd_out, STDOUT_FILENO) == -1) 
        {
          perror("dup2 output");
          close(fd_out);
          _exit(1);
        }

        //closing original file descriptor after duplication
        close(fd_out);
      }
      
      //handling error redirection
      if (cmd->error_file != NULL) 
      {
        //opening error file for writing (creating if needed, truncating if exists)
        int fd_err = open(cmd->error_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_err == -1) 
        {
          //file cannot be opened - printing error
          perror(cmd->error_file);
          _exit(1);
        }
        //redirecting stderr (fd 2) to the error file
        if (dup2(fd_err, STDERR_FILENO) == -1) 
        {
          perror("dup2 error");
          close(fd_err);
          _exit(1);
        }

        //closing original file descriptor after duplication
        close(fd_err);
      }
      
      //executing command with execvp
      //execvp searches for the program in PATH and executes it
      //first argument: program name, second argument: NULL-terminated args array
      execvp(cmd->command, cmd->args);
      
      //if execvp returns, it failed
      //exiting with 127 for command not found (standard shell convention)
      perror(cmd->command);
      _exit(127);
      
    default:
      //parent process - waiting for child to finish
      
      //waiting for child process to terminate
      waitpid(pid, &status, 0);
      
      //checking if child exited normally
      if (WIFEXITED(status)) 
      {
        //child exited normally
        //checking exit status (0 = success, non-zero = error)
        if (WEXITSTATUS(status) != 0) 
        {
          //command failed with non-zero exit status
          //error message already printed by child
        }
      }
      
      break;
  }
}
