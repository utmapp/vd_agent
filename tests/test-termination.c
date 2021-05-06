/* test-termination.c tests program terminates correctly sending SIGTERM
 *
 * Copyright 2020 Red Hat, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

static pid_t child_pid;
static char socket_name[1024 + 64];

static void cleanup(void)
{
    if (child_pid) {
        kill(child_pid, SIGKILL);
    }
    if (socket_name[0]) {
        unlink(socket_name);
    }
}

static void handle_alarm(int sig)
{
    fprintf(stderr, "Alarm reached!\n");
    exit(1);
}

static void check(int line, const char *cond_str, int cond_value)
{
    if (!cond_value) {
        alarm(0);
        fprintf(stderr, "%d: Check %s failed!\n", line, cond_str);
        exit(1);
    }
}
#define check(cond) check(__LINE__, #cond, cond)

static pid_t get_daemon_pid(void)
{
    pid_t res = 0;
    FILE *f = popen("ps -efww", "r");
    check(f != NULL);
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, socket_name) != NULL) {
            int pid;
            check(sscanf(line, "%*s %d", &pid) == 1);
            res = pid;
        }
    }
    pclose(f);
    return res;
}

int main(int argc, char **argv)
{
    atexit(cleanup);

    // create new filename for the socket
    char cwd[1024];
    check(getcwd(cwd, sizeof(cwd)) != NULL);
    snprintf(socket_name, sizeof(socket_name), "%s/sock-%u", cwd, (unsigned) getpid());

    // daemon should not exist now
    check(get_daemon_pid() == 0);

    // launch daemon with -S
    int pipes[2];
    check(pipe(pipes) == 0);
    child_pid = fork();
    check(child_pid != -1);
    if (child_pid == 0) {
        close(pipes[0]);
        execl("src/spice-vdagentd", "spice-vdagentd", "-S", socket_name, NULL);
        fprintf(stderr, "Error launching agent\n");
        return 1;
    }
    close(pipes[1]);
    pipes[1] = -1;

    // test child exits with success after a bit (wait timed)
    int status;
    signal(SIGALRM, handle_alarm);
    alarm(1);
    check(waitpid(child_pid, &status, 0) == child_pid);
    alarm(0);
    check(WIFEXITED(status) != 0);
    check(WEXITSTATUS(status) == 0);
    child_pid = 0;

    // test child created the socket passed
    struct stat sb;
    check(stat(socket_name, &sb) == 0);
    check((sb.st_mode & S_IFMT) == S_IFSOCK);

    // child should have created the daemon
    child_pid = get_daemon_pid();
    check(child_pid != 0);

    // wait a bit (1 second) and test daemon is still there, not failed some initialization
    sleep(1);
    check(get_daemon_pid() != 0);

    // send a SIGTERM, process should close in a bit
    kill(child_pid, SIGTERM);

    // test daemon exits after a while, we use the pipe we created above to
    // check if daemon closed
    alarm(1);
    check(read(pipes[0], cwd, sizeof(cwd)) == 0);
    alarm(0);
    check(get_daemon_pid() == 0);

    // test the socket disappeared, so we know the program exited successful
    check(stat(socket_name, &sb) == -1);
    check(errno == ENOENT);

    // don't send a SIGKILL to a not existing process
    child_pid = 0;

    return 0;
}
