#ifndef MYSHELL_H
#define MYSHELL_H

#include <stdio.h>    //printf(), fgets(), perror()
#include <stdlib.h>   //exit(), EXIT_SUCCESS, EXIT_FAILURE
#include <string.h>   //strcmp(), strlen(), strtok()
#include <unistd.h>   //fork(), execvp(), dup2()
#include <sys/wait.h> //wait(), waitpid()
#include <fcntl.h>    //open(), O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC
#include <errno.h>    //errno, ENOENT

//constants defining limits and shell prompt
#define MAX_INPUT 1024  //maximum input buffer size
#define MAX_ARGS 64     //maximum number of arguments
#define PROMPT "$ "     //shell prompt
#define MAX_CMDS 16     //maximum commands in a pipeline

//structure for holding parsed command information
typedef struct 
{
  char *command;        //command name
  char *args[MAX_ARGS]; //arguments array (null-terminated)
  char *input_file;     //input redirection file (null if none)
  char *output_file;    //output redirection file (null if none)
  char *error_file;     //error redirection file (null if none)
  int has_error;        //flag indicating parsing error
  char error_msg[256];  //error message if has_error is set
} Command;

//structure for holding pipeline of multiple commands
typedef struct
{
  Command cmds[MAX_CMDS]; //array of commands in pipeline
  int count;              //number of commands in pipeline
  int has_error;          //pipeline-level parse error
  char error_msg[256];    //error message for pipeline
} Pipeline;

//function declarations

//parser functions
int is_redirection_operator(char *token);
void parse_command(char *input, Command *cmd);
void parse_pipeline(char *input, Pipeline *p);

//executor functions
int validate_command(Command *cmd);
void execute_command(Command *cmd);
int validate_pipeline(Pipeline *p);
void execute_pipeline(Pipeline *p);

//builtin functions
int is_builtin(char *command);
int execute_builtin(Command *cmd);

#endif //MYSHELL_H
