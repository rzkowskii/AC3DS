#include "system.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

void ac3ds_read_launch_arguments(int &argc, char **&argv)
{
    FILE *file = fopen("sdmc:/3ds/assaultcube/server.args", "r");
    if(!file) return;

    // Each line is a single argument, including spaces in values such as -n.
    static std::vector<std::string> arguments;
    static std::vector<char *> merged;
    char line[1024];
    while(arguments.size() < 64 && fgets(line, sizeof(line), file))
    {
        size_t length = strlen(line);
        if(length == sizeof(line) - 1 && line[length - 1] != '\n')
        {
            int ch;
            do { ch = fgetc(file); } while(ch != EOF && ch != '\n');
            continue;
        }
        char *argument = line + strspn(line, " \t");
        while(length && strchr(" \t\r\n", line[length - 1])) line[--length] = '\0';
        if(argument < line + length && argument[0] == '-' && argument[1])
            arguments.emplace_back(argument);
    }
    fclose(file);
    if(arguments.empty()) return;

    // Explicit loader arguments take precedence over the SD file.
    static char program[] = "assaultcube";
    merged.push_back(argc > 0 ? argv[0] : program);
    for(std::string &argument : arguments) merged.push_back(&argument[0]);
    for(int i = 1; i < argc; ++i) merged.push_back(argv[i]);
    argc = static_cast<int>(merged.size());
    merged.push_back(nullptr);
    argv = merged.data();
}
