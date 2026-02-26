#include "myshell.h"

//checking if command is a built-in
//returns 1 if built-in, 0 otherwise
int is_builtin(char *command) 
{
  if (command == NULL) 
  {
    return 0;
  }
  //checking for built-in commands (cd, pwd, and echo)
  if (strcmp(command, "cd") == 0 || strcmp(command, "pwd") == 0 || strcmp(command, "echo") == 0) 
  {
    return 1;
  }

  return 0;
}

//executing built-in commands
//returns 0 on success, 1 on failure
int execute_builtin(Command *cmd) 
{
  //handling cd command for changing directory
  if (strcmp(cmd->command, "cd") == 0) 
  {
    char *path; //path to change to
    
    //checking if path argument was provided
    if (cmd->args[1] == NULL) 
    {
      //no argument provided - changing to home directory
      path = getenv("HOME");
      if (path == NULL) 
      {
        fprintf(stderr, "cd: HOME not set\n");
        return 1;
      }
    } 
    else 
    {
      //using provided path argument
      path = cmd->args[1];
    }
    
    //attempting to change directory
    if (chdir(path) != 0) 
    {
      perror("cd");
      return 1;
    }
    return 0;
  }
  
  //handling pwd command for printing working directory
  if (strcmp(cmd->command, "pwd") == 0) 
  {
    char cwd[1024]; //buffer for current working directory
    
    //getting current working directory
    if (getcwd(cwd, sizeof(cwd)) == NULL) 
    {
      perror("pwd");
      return 1;
    }
    
    //printing current working directory to stdout
    printf("%s\n", cwd);
    return 0;
  }
  
  //handling echo command for printing text
  if (strcmp(cmd->command, "echo") == 0) 
  {
    int interpret_escapes = 0; //flag for -e option
    int start_arg = 1; //starting argument index
    
    //checking for -e flag
    if (cmd->args[1] != NULL && strcmp(cmd->args[1], "-e") == 0) 
    {
      interpret_escapes = 1;
      start_arg = 2;
    }
    
    //printing each argument separated by spaces
    for (int i = start_arg; cmd->args[i] != NULL; i++) 
    {
      if (i > start_arg) 
      {
        printf(" ");
      }
      
      char *str = cmd->args[i];
      if (interpret_escapes) 
      {
        //processing escape sequences
        for (int j = 0; str[j] != '\0'; j++) 
        {
          if (str[j] == '\\' && str[j+1] != '\0') 
          {
            //handling escape sequences
            j++;
            switch (str[j]) 
            {
              case 'n': printf("\n"); break;
              case 't': printf("\t"); break;
              case 'r': printf("\r"); break;
              case '\\': printf("\\"); break;
              default: printf("%c", str[j]); break;
            }
          } 
          else 
          {
            printf("%c", str[j]);
          }
        }
      } 
      else 
      {
        //printing without interpreting escapes
        printf("%s", str);
      }
    }
    printf("\n");
    return 0;
  }
  
  //unknown built-in command (should not reach here)
  fprintf(stderr, "Unknown built-in: %s\n", cmd->command);
  return 1;
}
