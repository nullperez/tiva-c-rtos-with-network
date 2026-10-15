#ifndef NET_SHELL_H_
#define NET_SHELL_H_

// Network shell commands, runs in netTask through netCallSync()
void netShellCommand(void* line);

#endif
