#include "myshell.h"

//checking if a token is a redirection operator
//returns 1 if it's a redirection operator, 0 otherwise
int is_redirection_operator(char *token) 
{
  if (token == NULL) 
  {
    return 0;
  }
  //checking if token is <, >, or 2>
  if (strcmp(token, "<") == 0 || strcmp(token, ">") == 0 || strcmp(token, "2>") == 0) 
  {
    return 1;
  }

  return 0;
}

//parsing input string into command structure
void parse_command(char *input, Command *cmd) 
{
  int arg_index = 0; //index for arguments array
  char *token; //current token being processed
  
  //initializing command structure with NULL values
  cmd->command = NULL;
  cmd->input_file = NULL;
  cmd->output_file = NULL;
  cmd->error_file = NULL;
  cmd->has_error = 0; //no error initially
  cmd->error_msg[0] = '\0'; //empty error message
  
  //initializing all arguments array elements to NULL
  for (int i = 0; i < MAX_ARGS; i++) 
  {
    cmd->args[i] = NULL;
  }
  
  //tokenizing input string by spaces
  token = strtok(input, " ");
  
  //iterating through all tokens
  while (token != NULL && arg_index < MAX_ARGS - 1) 
  {
    
    //checking for input redirection operator
    if (strcmp(token, "<") == 0) 
    {
      //checking for duplicate input redirection
      if (cmd->input_file != NULL) 
      {
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Duplicate input redirection.");
        return;
      }
      //getting next token as input filename
      token = strtok(NULL, " ");
      if (token == NULL) 
      {
        //missing input file
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Input file not specified.");
        return;
      }
      //checking if token is a redirection operator instead of a filename
      if (is_redirection_operator(token)) 
      {
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Input file not specified.");
        return;
      }
      cmd->input_file = token; //storing input file

    }
    //checking for error redirection operator
    else if (strcmp(token, "2>") == 0) 
    {
      //checking for duplicate error redirection
      if (cmd->error_file != NULL) 
      {
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Duplicate error redirection.");
        return;
      }
      //getting next token as error filename
      token = strtok(NULL, " ");
      if (token == NULL) 
      {
        //missing error file
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Error output file not specified.");
        return;
      }
      //checking if token is a redirection operator instead of a filename
      if (is_redirection_operator(token)) 
      {
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Error output file not specified.");
        return;
      }
      cmd->error_file = token; //storing error file
    }
    //checking for output redirection operator
    else if (strcmp(token, ">") == 0) 
    {
      //checking for duplicate output redirection
      if (cmd->output_file != NULL) 
      {
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Duplicate output redirection.");
        return;
      }
      //getting next token as output filename
      token = strtok(NULL, " ");
      if (token == NULL) 
      {
        //missing output file
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Output file not specified after redirection.");
        return;
      }
      //checking if token is a redirection operator instead of a filename
      if (is_redirection_operator(token)) 
      {
        cmd->has_error = 1;
        strcpy(cmd->error_msg, "Output file not specified after redirection.");
        return;
      }
      cmd->output_file = token; //storing output file
    }
    //handling regular command and arguments
    else 
    {
      //setting first token as command name
      if (cmd->command == NULL) 
      {
        cmd->command = token;
      }
      //adding token to arguments array
      cmd->args[arg_index] = token;
      arg_index++;
    }
    
    //getting next token
    token = strtok(NULL, " ");
  }
  
  //checking if we exited loop due to exceeding max arguments
  //if yes, check if there are remaining tokens
  if (arg_index >= MAX_ARGS - 1) 
  {
    //checking if there are more tokens remaining
    char *remaining = strtok(NULL, " ");
    if (remaining != NULL) 
    {
      //warning user about exceeding maximum arguments
      fprintf(stderr, "Warning: Maximum number of arguments (%d) exceeded. Extra arguments ignored.\n", MAX_ARGS - 1);
    }
  }
  
  //null-terminating the arguments array for execvp()
  cmd->args[arg_index] = NULL;
}

//trimming leading and trailing whitespace from a string in-place
//returns pointer to the trimmed string
static char *trim_inplace(char *s)
{
  if (!s) return s;

  //trimming leading spaces and tabs
  while (*s == ' ' || *s == '\t') s++;

  //trimming trailing spaces and tabs
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) 
  {
    s[len - 1] = '\0';
    len--;
  }
  return s;
}

//parsing pipeline of commands separated by pipe operator
void parse_pipeline(char *input, Pipeline *p)
{
  //initializing pipeline structure
  p->count = 0;
  p->has_error = 0;
  p->error_msg[0] = '\0';

  //splitting input by '|' in-place while detecting errors
  //detecting: command1 | (missing after pipe)
  //detecting: command1 | | command2 (empty between pipes)
  char *cursor = input;
  char *segment_start = input;

  while (1) 
  {
    if (*cursor == '|' || *cursor == '\0') 
    {
      char saved = *cursor;
      *cursor = '\0';

      char *seg = trim_inplace(segment_start);

      if (seg[0] == '\0') 
      {
        //empty segment detected
        if (saved == '\0') 
        {
          //input ends with pipe or empty line
          p->has_error = 1;
          strcpy(p->error_msg, "Command missing after pipe.");
          return;
        } 
        else 
        {
          //consecutive pipes detected
          p->has_error = 1;
          strcpy(p->error_msg, "Empty command between pipes.");
          return;
        }
      }

      if (p->count >= MAX_CMDS) 
      {
        p->has_error = 1;
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "Error: Too many commands in pipeline (max %d).", MAX_CMDS);
        return;
      }

      //parsing this segment into a command
      parse_command(seg, &p->cmds[p->count]);

      //checking if parse_command found an error and bubbling it up
      if (p->cmds[p->count].has_error) 
      {
        p->has_error = 1;
        strncpy(p->error_msg, p->cmds[p->count].error_msg, sizeof(p->error_msg) - 1);
        p->error_msg[sizeof(p->error_msg) - 1] = '\0';
        return;
      }

      p->count++;

      if (saved == '\0') 
      {
        break; //reaching end of input
      }

      //moving to next segment
      segment_start = cursor + 1;
      cursor = segment_start;
      continue;
    }

    cursor++;
  }
}

