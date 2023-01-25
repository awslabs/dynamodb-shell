// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0
//
#include <iostream>

#define TEST_HARNESS 1
#include "logging.hpp"
#include "allocation.hpp"

namespace ddbsh
{
    class CBaz
    {
    public:
        CBaz() {
            std::cout << "In CBaz()" << std::endl;
            secret1 = NEW char[10];
            secret2 = NEW std::string;
            somestring = STRDUP((char *) "Hello");
            std::cout << "Done CBaz()" << std::endl;
        };

        ~CBaz() {
            printf("In ~CBaz(), secret1 = %p, secret2 = %p, string = %p\n", secret1, secret2, somestring);
            delete [] secret1;
            delete secret2;
            FREE(somestring);
            std::cout << "Done ~CBaz()" << std::endl;
        };

    private:
        char * secret1;
        char * somestring;
        std::string * secret2;
    };

    class CBar
    {
    public:
        CBar() {
            std::cout << "In CBar()" << std::endl;
            secret = new char[100];
            cbaz = NEW CBaz();
            somestring = STRDUP((char *)"Hello");
            std::cout << "Done CBar()" << std::endl;
        };

        ~CBar() {
            std::cout << "In ~CBar()" << std::endl;
            delete [] secret;
            delete cbaz;
            FREE(somestring);
            std::cout << "Done ~CBar()" << std::endl;
        };

        char * secret;
        CBaz * cbaz;
        char * somestring;
    };

    class CFoo
    {
    public:
        CFoo() {
            std::cout << "In CFoo()" << std::endl;
        };

        ~CFoo() {
            std::cout << "In ~CFoo()" << std::endl;
        };

        CBar bar;
    };
}

using namespace ddbsh;
int main ()
{
    setdebug(DDBSH_LOGLEVEL_DEBUG);
    std::cout << "In main ()" << std::endl;

    char * str1 = (char *) MALLOC(500);
    char * str2 = (char *) CALLOC(1000, 1);
    char * str3 = STRDUP((char *) "foobar");
    char * str4 = (char *) MALLOC(100);

    str4 = (char *) REALLOC(str4, 200);

    printf("Done with four allocations, and one reallocation. [%p, %p, %p, %p]\n",
           str1, str2, str3, str4);
    show_allocations();

    CFoo * foo = NEW CFoo();
    printf("Done with new CFoo(). foo = %p\n", foo);
    show_allocations();

    delete foo;
    printf("Done with delete foo\n");
    show_allocations();

    FREE(str1);
    FREE(str2);
    FREE(str3);
    FREE(str4);
    printf("Done with four frees.\n");
    show_allocations();

    return 0;
}

