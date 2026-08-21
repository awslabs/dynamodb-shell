// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ddbsh.hpp"
#include "input-source.hpp"
#include "logging.hpp"
#include "version.h"
#include "version.hpp"

using namespace ddbsh;

void ctrlc(int ignored)
{
    return;
}

typedef struct
{
    bool quiet;
    bool abort_on_error;
    char * inputfile;
    char * cmdstring;

#ifdef TEST_HARNESS
    bool sort;
    char * expected;
    char * record;

    // the three below are NOT from the command line.
    // saved_* hold dup()'d file descriptors for the real stdout/stderr so
    // revert_output() can restore them after freopen() redirection.
    int saved_stdout_fd;
    int saved_stderr_fd;
    char * recordint;
#endif
} cmdline_t;

// will exit on error, ok to return void
static void checkopts(cmdline_t * cmdline)
{
    if (cmdline->inputfile &&cmdline->cmdstring)
    {
        printf("ddbsh: Cannot specify both -c and -f.\n");
        exit(1);
    }

#ifdef TEST_HARNESS
    if (cmdline->expected && cmdline->record)
    {
        printf("ddbsh: Cannot specify both -T and -R.\n");
        exit(1);
    }
#endif
}

// will exit on error, ok to return void
static void cmdparse(cmdline_t * cmdline, int ac, char ** av)
{
    int ch;

#ifdef TEST_HARNESS
    cmdline->quiet = true;

    while ((ch = getopt(ac, av, "qc:f:eT:R:sd")) != -1)
#else
    while ((ch = getopt(ac, av, "qc:f:ed")) != -1)
#endif
    {
        switch(ch)
        {
        case 'd':
            setdebug(DDBSH_LOGLEVEL_DEBUG);
            break;

        case 'q':
            cmdline->quiet = true;
            break;

        case 'e':
            cmdline->abort_on_error = true;
            break;

        case 'c':
            if (cmdline->cmdstring)
                free(cmdline->cmdstring);

            cmdline->cmdstring = strdup(optarg);
            break;

        case 'f':
            if (cmdline->inputfile)
                free(cmdline->inputfile);

            cmdline->inputfile = strdup(optarg);
            break;

#ifdef TEST_HARNESS
        case 'T':
            if (cmdline->expected)
                free(cmdline->expected);

            cmdline->expected = strdup(optarg);
            break;

        case 'R':
            if (cmdline->record)
                free(cmdline->record);

            cmdline->record = strdup(optarg);
            break;

        case 's':
            cmdline->sort = true;
            break;
#endif

        default:
            printf("Unknown command line option %c\n", ch);
            exit(1);
        }
    }

    // perform validations.
    checkopts(cmdline);
}

static void init()
{
    if (getenv("DDBSH_LOGLEVEL_DEBUG"))
        setdebug(DDBSH_LOGLEVEL_DEBUG);
    else
        setdebug(DDBSH_LOGLEVEL_DEFAULT);

    logtofile(NULL);

    signal(SIGINT, ctrlc);
}

#ifdef TEST_HARNESS
static void capture_output(cmdline_t * cmdline)
{
    if (!cmdline->record && !cmdline->expected)
        return;

    // Decide where captured output goes. If the user asked for recording
    // with no sorting, write straight into the output file; otherwise use an
    // intermediate temp file that process_recording() will sort/diff.
    const char * target;
    if (cmdline->record && !cmdline->sort)
    {
        target = cmdline->record;
    }
    else
    {
        cmdline->recordint = mktemp(strdup((char *)"/tmp/ddbsh-recXXXXXXX"));
        target = cmdline->recordint;
    }

    // Redirect stdout/stderr into the capture file using freopen() rather
    // than reassigning the stdout/stderr pointers. On some libc
    // implementations (notably glibc, where stdout is copy-relocated into the
    // executable), printf() writes through libc's internal stream object, so
    // assigning a new FILE* to the application's `stdout` pointer does NOT
    // redirect output -- the capture file stays empty and the data leaks to
    // the terminal. freopen() rebinds the underlying stream and works
    // portably. We dup() the real fds first so revert_output() can restore
    // them for process_recording()'s own output.
    fflush(stdout);
    fflush(stderr);

    cmdline->saved_stdout_fd = dup(fileno(stdout));
    cmdline->saved_stderr_fd = dup(fileno(stderr));

    if (cmdline->saved_stdout_fd == -1 || cmdline->saved_stderr_fd == -1)
    {
        perror("dup");
        exit(1);
    }

    if (freopen(target, "w", stdout) == NULL)
    {
        perror(target);
        exit(1);
    }

    // send stderr to the same file as stdout
    if (dup2(fileno(stdout), fileno(stderr)) == -1)
    {
        perror("dup2");
        exit(1);
    }
}

static void revert_output(cmdline_t * cmdline)
{
    if (cmdline->record || cmdline->expected)
    {
        // Flush the captured data into the file, then rebind stdout/stderr
        // back to the real descriptors saved in capture_output(). We do NOT
        // fclose(stdout) here: it remains the standard stream, just pointing
        // at a different fd.
        fflush(stdout);
        fflush(stderr);

        dup2(cmdline->saved_stdout_fd, fileno(stdout));
        dup2(cmdline->saved_stderr_fd, fileno(stderr));

        close(cmdline->saved_stdout_fd);
        close(cmdline->saved_stderr_fd);

        clearerr(stdout);
        clearerr(stderr);
    }
}

static int execute_command (char * cmd)
{
    return system(cmd);
};

static int sort_file(char * infile, char * outfile)
{
    char cmd[2000+1];
    int retval;

    snprintf(cmd, sizeof(cmd) - 1, "LC_ALL=C sort -o %s %s", infile, outfile);
    logdebug("[%s, %d] sorting data %s\n", __FILENAME__, __LINE__, cmd);
    retval = execute_command (cmd);

    return retval ? 1 : 0;
}

static int copy_file(char * infile, char * outfile)
{
    char cmd[2000+1];
    int retval;

    snprintf(cmd, sizeof(cmd) - 1, "cp %s %s", infile, outfile);
    logdebug("[%s, %d] copying data %s\n", __FILENAME__, __LINE__, cmd);
    retval = execute_command(cmd);

    return retval ? 1 : 0;
}

static int diff_file(char * afile, char * bfile, char * cfile)
{
    char cmd[2000+1];
    int retval;

    snprintf(cmd, sizeof(cmd) - 1, "diff %s %s > %s", afile, bfile, cfile);
    logdebug("[%s, %d] comparing data %s\n", __FILENAME__, __LINE__, cmd);
    retval = execute_command(cmd);

    return retval ? 1 : 0;
}

static int process_recording(cmdline_t * cmdline)
{
    // this is a case where we want to check
    // sort the file in place and then compare.
    char cmd[2000+1];
    int retval;

    // if we are being asked to test (and are provided
    // cmdline->expected) but the file doesn't exist and the
    // environment variable DDBSH_RECORD_IF_MISSING is set, treat it
    // as just a recording.

    if (access(cmdline->expected, R_OK) && getenv("DDBSH_RECORD_IF_MISSING"))
    {
        if (cmdline->sort)
            retval = sort_file(cmdline->expected, cmdline->recordint);
        else
            retval = copy_file(cmdline->recordint, cmdline->expected);

        if (retval == 0)
            printf("Passed\n");
        else
            printf("\nFailed\n");

        unlink(cmdline->recordint);
    }
    else
    {
        char * testresult = mktemp(strdup((char *)"/tmp/ddbsh-recXXXXXXX"));

        if (cmdline->sort)
        {
            if ((retval = sort_file(cmdline->recordint, cmdline->recordint)) == 0)
                retval = diff_file(cmdline->recordint, cmdline->expected, testresult);
        }
        else
        {
            retval = diff_file(cmdline->recordint, cmdline->expected, testresult);
        }

        if (retval == 0)
            printf("Passed\n");
        else
        {
            FILE * testr;

            if ((testr = fopen(testresult, "r")) == (FILE *) NULL)
            {
                perror("fopen()");
                return 1;
            }

            while(!feof(testr))
            {
                int ch;
                if ((ch = fgetc(testr)) != EOF)
                    putchar(ch);
            }

            fclose(testr);

            printf("\nFailed\n");
        }

        unlink(cmdline->recordint);
        unlink(testresult);
    }

    return retval ? 1:0;
}

#endif

int main(int ac, char ** av)
{
    int retval;

    CDDBSh * ddbsh = NULL;

    init();

    cmdline_t cmdline;
    memset(&cmdline, 0, sizeof(cmdline_t));

    cmdparse(&cmdline, ac, av);

    ddbsh = CDDBSh::getInstance();

    // load the configuration file
    ddbsh->runinit();

#ifdef TEST_HARNESS
    capture_output(&cmdline);
#endif

    if (!cmdline.quiet)
        printf("ddbsh - version %s\n", ddbsh_version());

    ddbsh->abort_on_error(cmdline.abort_on_error);

    if (cmdline.inputfile)
        retval = ddbsh->runfile(cmdline.inputfile);
    else if (cmdline.cmdstring)
        retval = ddbsh->runcommand(cmdline.cmdstring);
    else
        retval = ddbsh->runstdin();

#ifndef TEST_HARNESS
    // if this is not the test harness, we're all done here
    return retval ? 1 : 0;
#else
    // get the regular stdout back
    revert_output(&cmdline);

    if (retval || (!cmdline.record && !cmdline.expected))
        return retval ? 1 : 0;

    if (cmdline.record && cmdline.sort)
        return sort_file(cmdline.record, cmdline.recordint);
    else if (cmdline.record)
        return 0;

    assert(cmdline.expected && cmdline.recordint);

    return process_recording(&cmdline);
#endif
}

