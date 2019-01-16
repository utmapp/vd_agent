/*  test-file-xfers.c  - test file transfer

    Copyright 2019 Red Hat, Inc.

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
#include <string.h>
#include <unistd.h>
#include <glib.h>

#include <spice/vd_agent.h>

#include "file-xfers.h"

static void test_file(const char *file_name, const char *out)
{
    char *fn = g_strdup(file_name);
    int fd = vdagent_file_xfers_create_file("./test-dir", &fn);
    if (out) {
        g_assert_cmpint(fd, !=, -1);
        g_assert_cmpstr(fn, ==, out);
        close(fd);
        g_assert_cmpint(access(out, W_OK), ==, 0);
    } else {
        g_assert_cmpint(fd, ==, -1);
    }
    g_free(fn);
}

int main(int argc, char *argv[])
{
    assert(system("rm -rf test-dir && mkdir test-dir") == 0);

    // create a file
    test_file("test.txt", "./test-dir/test.txt");

    // create a file with an existing name
    for (int i = 1; i < 64; ++i) {
        char out_name[64];
        sprintf(out_name, "./test-dir/test (%d).txt", i);
        test_file("test.txt", out_name);
    }

    // check too much files with the same name
    test_file("test.txt", NULL);

    // create a file in a subdirectory not existing
    test_file("subdir/test.txt", "./test-dir/subdir/test.txt");

    // create a file in a directory with no permissions
    assert(system("ln -s /proc/1 test-dir/baddir") == 0);
    test_file("baddir/test2.txt", NULL);

    // try to create a file with a path where there's a file (should fail)
    test_file("test.txt/out", NULL);

    // create a file without extension in a directory with extension
    test_file("sub.dir/test", "./test-dir/sub.dir/test");

    // create a file with same name above, should not strip the filename
    test_file("sub.dir/test", "./test-dir/sub.dir/test (1)");

    assert(system("rm -rf test-dir") == 0);

    return 0;
}
