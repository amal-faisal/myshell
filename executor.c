#include "myshell.h"

//validating parsed command before execution
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
    //attempting to open file for reading to verify existence
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

//executing command in a child process
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
      //first argument: program name, second argument: null-terminated args array
      execvp(cmd->command, cmd->args);
      
      //if execvp returns, it failed
      //exiting with 127 for command not found (standard shell convention)
      if (errno == ENOENT) 
      {
        fprintf(stderr, "Command not found.\n");
      } 
      else 
      {
        perror(cmd->command);
      }
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

//checking if command exists in PATH or as executable path
//returns 1 if command exists, 0 otherwise
static int command_exists(const char *cmd)
{
  if (!cmd || cmd[0] == '\0') return 0;

  //if command contains '/', treating as a path (./prog, /bin/ls, etc.)
  if (strchr(cmd, '/')) 
  {
    return (access(cmd, X_OK) == 0);
  }

  //searching PATH environment variable
  const char *path_env = getenv("PATH");
  if (!path_env) return 0;

  //making a copy because strtok modifies the string
  char path_copy[4096];
  strncpy(path_copy, path_env, sizeof(path_copy) - 1);
  path_copy[sizeof(path_copy) - 1] = '\0';

  char *dir = strtok(path_copy, ":");
  while (dir)
  {
    char full[4096];
    snprintf(full, sizeof(full), "%s/%s", dir, cmd);
    if (access(full, X_OK) == 0) return 1;
    dir = strtok(NULL, ":");
  }
  return 0;
}

//validating pipeline of commands before execution
//returns 1 if valid, 0 if invalid
int validate_pipeline(Pipeline *p)
{
  if (p->has_error) 
  {
    fprintf(stderr, "Error: %s\n", p->error_msg);
    return 0;
  }

  if (p->count <= 0) 
  {
    fprintf(stderr, "Error: No command specified.\n");
    return 0;
  }

  //validating each command in the pipeline
  for (int i = 0; i < p->count; i++) 
  {
    Command *c = &p->cmds[i];

    //checking for parse-level error (should already be bubbled up)
    if (c->has_error) 
    {
      fprintf(stderr, "Error: %s\n", c->error_msg);
      return 0;
    }

    if (c->command == NULL) 
    {
      fprintf(stderr, "Error: No command specified.\n");
      return 0;
    }

    //checking input file existence (if specified)
    if (c->input_file != NULL) 
    {
      int fd = open(c->input_file, O_RDONLY);
      if (fd == -1) 
      {
        perror(c->input_file);
        return 0;
      }
      close(fd);
    }

    //validating command existence for pipeline sequences
    //allowing builtins too as they exist even if not in PATH
    if (!is_builtin(c->command) && !command_exists(c->command)) 
    {
      if (p->count == 1) 
      {
        fprintf(stderr, "Command not found.\n");
      } 
      else 
      {
        fprintf(stderr, "Command not found in pipe sequence.\n");
      }
      return 0;
    }
  }

  return 1;
}

//executing pipeline of commands connected by pipes
void execute_pipeline(Pipeline *p)
{
  int n = p->count;

  //creating n-1 pipes for connecting commands
  int pipes[MAX_CMDS - 1][2];
  for (int i = 0; i < n - 1; i++) 
  {
    if (pipe(pipes[i]) == -1) 
    {
      perror("pipe");
      return;
    }
  }

  pid_t pids[MAX_CMDS];

  //forking and setting up each command in the pipeline
  for (int i = 0; i < n; i++) 
  {
    pid_t pid = fork();
    if (pid == -1) 
    {
      perror("fork failed");
      //closing pipes in parent on failure
      for (int k = 0; k < n - 1; k++) 
      {
        close(pipes[k][0]);
        close(pipes[k][1]);
      }
      return;
    }

    if (pid == 0) 
    {
      //child process
      
      //connecting stdin to previous pipe read end if not first command
      if (i > 0) 
      {
        if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1) 
        {
          perror("dup2 pipe stdin");
          _exit(1);
        }
      }

      //connecting stdout to current pipe write end if not last command
      if (i < n - 1) 
      {
        if (dup2(pipes[i][1], STDOUT_FILENO) == -1) 
        {
          perror("dup2 pipe stdout");
          _exit(1);
        }
      }

      //closing all pipe file descriptors in child after dup2
      for (int k = 0; k < n - 1; k++) 
      {
        close(pipes[k][0]);
        close(pipes[k][1]);
      }

      Command *c = &p->cmds[i];

      //applying redirections after pipe hookups so redirection overrides pipe
      if (c->input_file != NULL) 
      {
        int fd_in = open(c->input_file, O_RDONLY);
        if (fd_in == -1) 
        { 
          perror(c->input_file); 
          _exit(1); 
        }
        if (dup2(fd_in, STDIN_FILENO) == -1) 
        { 
          perror("dup2 input"); 
          close(fd_in); 
          _exit(1); 
        }
        close(fd_in);
      }

      if (c->output_file != NULL) 
      {
        int fd_out = open(c->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out == -1)
        { 
          perror(c->output_file); 
          _exit(1); 
        }
        if (dup2(fd_out, STDOUT_FILENO) == -1)
        { 
          perror("dup2 output"); 
          close(fd_out); 
          _exit(1); 
        }
        close(fd_out);
      }

      if (c->error_file != NULL) 
      {
        int fd_err = open(c->error_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_err == -1)
        { 
          perror(c->error_file); 
          _exit(1); 
        }
        if (dup2(fd_err, STDERR_FILENO) == -1)
        { 
          perror("dup2 error"); 
          close(fd_err); 
          _exit(1); 
        }
        close(fd_err);
      }

      //executing builtins inside pipeline in child (subshell behavior)
      if (is_builtin(c->command)) 
      {
        int rc = execute_builtin(c);
        _exit(rc);
      }

      //executing external command with execvp
      execvp(c->command, c->args);

      //if execvp returns, it failed
      if (errno == ENOENT) 
      {
        fprintf(stderr, "Command not found.\n");
      } 
      else 
      {
        perror(c->command);
      }
      _exit(127);
    }

    //parent process - storing child pid
    pids[i] = pid;
  }

  //parent closing all pipe file descriptors
  for (int k = 0; k < n - 1; k++) 
  {
    close(pipes[k][0]);
    close(pipes[k][1]);
  }

  //waiting for all children so prompt returns correctly after pipeline completes
  for (int i = 0; i < n; i++) 
  {
    int status;
    waitpid(pids[i], &status, 0);
  }
}

