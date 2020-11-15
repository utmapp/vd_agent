/*  test-session-info.c  - test session info

    Copyright 2020 Red Hat, Inc.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <config.h>

#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "session-info.h"

int main(int argc, char *argv[])
{
    int pid, uid, uid_si;

    pid = (int)getpid();

    struct session_info *session_info = session_info_create(1);
    if (session_info == NULL) {
        return 1;
    }

    char *session = session_info_session_for_pid(session_info, pid);
    if (session == NULL) {
        session_info_destroy(session_info);
        return 2;
    }
    uid_si = session_info_uid_for_session(session_info, session);

    free(session);
    session_info_destroy(session_info);

    uid = getuid();
    printf("MAIN: uid is %d, uid_si is %d\n", uid, uid_si);

    if (uid != uid_si) {
        fprintf(stderr, "MAIN: uid (%d) does not match the uid "
                        "obtained from session info (%d)\n", uid, uid_si);
        return 3;
    }

    return 0;
}
