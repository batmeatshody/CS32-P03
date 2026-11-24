/*
 * main.c - gshell, the Gaucho Shell
 *
 * This is the only file you edit and submit. main() and the prompt loop
 * are done for you. Every other function below has a comment describing
 * what it does. Work through the checkpoints in the writeup in order:
 *
 *   Checkpoint 1: tokenize, parse_pipeline, and the exit built-in
 *   Checkpoint 2: run_command and run_pipeline for a single command
 *   Checkpoint 3: input and output redirection
 *   Checkpoint 4: pipelines
 */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/** The longest input line is 100 characters, plus 1 for the '\0' */
#define MAX_LINE 101

/** A 100-character line has at most 50 whitespace-separated tokens */
#define MAX_TOKENS 50

/** A pipeline can never have more commands than the line has tokens */
#define MAX_COMMANDS MAX_TOKENS

extern char **environ;

/**
 * One command of a pipeline. For example, the line
 *
 *     /bin/sort -r < in.txt | /usr/bin/head -n 3
 *
 * is a pipeline of two commands. The first has argv {"/bin/sort", "-r",
 * NULL}, input_file "in.txt", and output_file NULL.
 */
typedef struct {
    /** The executable path and its arguments, followed by NULL (as execve expects) */
    char *argv[MAX_TOKENS + 1];
    /** The file named after <, or NULL if the command has no input redirection */
    char *input_file;
    /** The file named after >, or NULL if the command has no output redirection */
    char *output_file;
} command_t;

/**
 * Splits `line` into its whitespace-separated tokens and stores a pointer
 * to each one in `tokens`. Changes the contents of `line`.
 *
 * Returns the number of tokens.
 *
 * __attribute__((unused)) stops the compiler from warning that a function
 * is never called. You can remove it once you call the function.
 */
static int tokenize(char *line, char *tokens[]) {
    const char *whitespace = " \t\n\r\f\v";
    char *saveptr;
    char *token = strtok_r(line, whitespace, &saveptr);
    int num_tokens = 0;

    while (token != NULL) {
        if (num_tokens == MAX_TOKENS) {
            return -1;
        }
        tokens[num_tokens++] = token;
        token = strtok_r(NULL, whitespace, &saveptr);
    }

    return num_tokens;
}

/**
 * Groups `tokens` into the commands of a pipeline and fills in one
 * command_t per command in `commands`.
 *
 * Returns the number of commands, or -1 if the tokens are not a valid
 * pipeline.
 */
static int parse_pipeline(char *tokens[], int num_tokens, command_t commands[]) {
    if (num_tokens <= 0) {
        return -1;
    }

    memset(commands, 0, sizeof(command_t) * MAX_COMMANDS);
    int command_index = 0;
    int argument_count = 0;
    int has_input = 0;
    int has_output = 0;

    for (int i = 0; i < num_tokens; i++) {
        if (strcmp(tokens[i], "|") == 0) {
            if (argument_count == 0 || command_index + 1 >= MAX_COMMANDS) {
                return -1;
            }
            commands[command_index].argv[argument_count] = NULL;
            command_index++;
            argument_count = 0;
            has_input = 0;
            has_output = 0;
        } else if (strcmp(tokens[i], "<") == 0 ||
                   strcmp(tokens[i], ">") == 0) {
            int is_input = strcmp(tokens[i], "<") == 0;
            if (i + 1 >= num_tokens ||
                strcmp(tokens[i + 1], "<") == 0 ||
                strcmp(tokens[i + 1], ">") == 0 ||
                strcmp(tokens[i + 1], "|") == 0 ||
                (is_input && has_input) || (!is_input && has_output)) {
                return -1;
            }

            if (is_input) {
                commands[command_index].input_file = tokens[++i];
                has_input = 1;
            } else {
                commands[command_index].output_file = tokens[++i];
                has_output = 1;
            }
        } else {
            if (argument_count >= MAX_TOKENS) {
                return -1;
            }
            commands[command_index].argv[argument_count++] = tokens[i];
        }
    }

    if (argument_count == 0) {
        return -1;
    }
    commands[command_index].argv[argument_count] = NULL;
    return command_index + 1;
}

/**
 * Starts `cmd` in a child process. The child reads its standard input
 * from `in_fd` and writes its standard output to `out_fd`, unless the
 * command redirects them to a file. Does not wait for the child.
 *
 * Returns the child's process ID, or -1 if no child was started.
 */
static pid_t run_command(command_t *cmd, int in_fd, int out_fd,
                         int close_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        int input_source = in_fd;
        int output_source = out_fd;
        int redirected_input = -1;
        int redirected_output = -1;

        if (cmd->input_file != NULL) {
            redirected_input = open(cmd->input_file, O_RDONLY);
            if (redirected_input < 0) {
                perror(cmd->input_file);
                _exit(1);
            }
            input_source = redirected_input;
        }
        if (cmd->output_file != NULL) {
            redirected_output =
                open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (redirected_output < 0) {
                perror(cmd->output_file);
                _exit(1);
            }
            output_source = redirected_output;
        }

        if (dup2(input_source, STDIN_FILENO) < 0) {
            perror("dup2");
            _exit(1);
        }
        if (dup2(output_source, STDOUT_FILENO) < 0) {
            perror("dup2");
            _exit(1);
        }

        int descriptors[] = {
            in_fd, out_fd, close_fd, redirected_input, redirected_output
        };
        for (size_t i = 0; i < sizeof(descriptors) / sizeof(descriptors[0]);
             i++) {
            if (descriptors[i] <= STDERR_FILENO) {
                continue;
            }
            int already_closed = 0;
            for (size_t j = 0; j < i; j++) {
                if (descriptors[j] == descriptors[i]) {
                    already_closed = 1;
                    break;
                }
            }
            if (!already_closed) {
                close(descriptors[i]);
            }
        }

        execve(cmd->argv[0], cmd->argv, environ);
        perror(cmd->argv[0]);
        _exit(127);
    }

    return pid;
}

/**
 * Runs the `num_commands` commands of a pipeline, with each command's
 * standard output connected to the next command's standard input. Then
 * waits for all of them and prints each one's exit status, in order.
 */
static void run_pipeline(command_t commands[], int num_commands) {
    pid_t pids[MAX_COMMANDS];
    int num_started = 0;
    int input_fd = STDIN_FILENO;

    for (int i = 0; i < num_commands; i++) {
        int output_pipe[2] = {-1, -1};
        int output_fd = STDOUT_FILENO;
        if (i + 1 < num_commands) {
            if (pipe(output_pipe) < 0) {
                perror("pipe");
                if (input_fd != STDIN_FILENO) {
                    close(input_fd);
                }
                break;
            }
            output_fd = output_pipe[1];
        }

        pid_t pid = run_command(&commands[i], input_fd, output_fd,
                                output_pipe[0]);
        if (pid < 0) {
            if (input_fd != STDIN_FILENO) {
                close(input_fd);
            }
            if (output_pipe[0] >= 0) {
                close(output_pipe[0]);
                close(output_pipe[1]);
            }
            break;
        }

        pids[num_started++] = pid;
        if (input_fd != STDIN_FILENO) {
            close(input_fd);
        }
        if (output_pipe[0] >= 0) {
            close(output_pipe[1]);
            input_fd = output_pipe[0];
        }
    }

    for (int i = 0; i < num_started; i++) {
        int status;
        pid_t waited;
        do {
            waited = waitpid(pids[i], &status, 0);
        } while (waited < 0 && errno == EINTR);

        if (waited < 0) {
            perror("waitpid");
            continue;
        }

        int exit_status = WIFEXITED(status) ? WEXITSTATUS(status)
                                             : 128 + WTERMSIG(status);
        printf("exit status: %d\n", exit_status);
    }
}

/**
 * Runs one line of input: either the exit built-in, or a pipeline of
 * commands. Prints "Invalid command" to stderr if the line is not a valid
 * pipeline.
 */
static void parse_and_run_command(char *line) {
    char *tokens[MAX_TOKENS];
    int num_tokens = tokenize(line, tokens);
    if (num_tokens == 1 && strcmp(tokens[0], "exit") == 0) {
        exit(0);
    }

    command_t commands[MAX_COMMANDS];
    int num_commands = parse_pipeline(tokens, num_tokens, commands);
    if (num_commands < 0) {
        fprintf(stderr, "Invalid command\n");
        return;
    }
    run_pipeline(commands, num_commands);
}

int main(void) {
    char line[MAX_LINE];

    while (1) {
        printf("> ");
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        // fgets keeps the '\n' at the end of the line. strcspn(line, "\n")
        // returns the index of the first '\n' in line (or of the final '\0'
        // if there is none), so this replaces the newline with '\0'.
        line[strcspn(line, "\n")] = '\0';
        parse_and_run_command(line);
    }

    return 0;
}
