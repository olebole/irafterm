#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#define _BSD_SOURCE
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <string.h>

#define START_GRAPH "\035\031"
#define CONT_GRAPH "\035\037"
#define END_GRAPH   "\030"

static int gfile;
int init_graphics() {
  gfile = open("logfile.dat", O_WRONLY | O_CREAT, 0644);
}

int handle_graphics(const void *buf, size_t count) {
  write(gfile, buf, count);
}


int main(int ac, char *av[]) {
  int fdm, fds;
  int rc;
  char input[150];
  int in_graph = 0;
  
  // Check arguments
  if (ac <= 1) {
    fprintf(stderr, "Usage: %s program_name [parameters]\n", av[0]);
    exit(1);
  }

  fdm = posix_openpt(O_RDWR);
  if (fdm < 0) {
    fprintf(stderr, "Error %d on posix_openpt()\n", errno);
    return 1;
  }

  rc = grantpt(fdm);
  if (rc != 0) {
    fprintf(stderr, "Error %d on grantpt()\n", errno);
    return 1;
  }

  rc = unlockpt(fdm);
  if (rc != 0) {
    fprintf(stderr, "Error %d on unlockpt()\n", errno);
    return 1;
  }

  // Open the slave side ot the PTY
  fds = open(ptsname(fdm), O_RDWR);

  // Create the child process
  if (fork()) {
    fd_set fd_in;
    // FATHER

    // Close the slave side of the PTY
    close(fds);

    while (1) {
      // Wait for data from standard input and master side of PTY
      FD_ZERO(&fd_in);
      FD_SET(0, &fd_in);
      FD_SET(fdm, &fd_in);
      
      rc = select(fdm + 1, &fd_in, NULL, NULL, NULL);
      switch(rc) {
      case -1 : fprintf(stderr, "Error %d on select()\n", errno);
	exit(1);

      default : {
        // If data on standard input
        if (FD_ISSET(0, &fd_in)) {
          rc = read(0, input, sizeof(input)-1);
          if (rc > 0) {
	    write(fdm, input, rc);
	  } else {
            if (rc < 0) {
              fprintf(stderr, "Error %d on read standard input\n", errno);
              exit(1);
            }
          }
        }

        // If data on master side of PTY
        if (FD_ISSET(fdm, &fd_in)) {
          rc = read(fdm, input, sizeof(input)-1);
          if (rc > 0) {
            // Send data on standard output
	    
	    input[rc] = '\0';
	    if (!in_graph) {
	      char *st = strstr(input, START_GRAPH);
	      if (st != NULL) {
		init_graphics();
	      } else {
		st = strstr(input, CONT_GRAPH);
	      }
	      if (st != NULL) {
		in_graph = 1;
		write(1, input, st-input);
		handle_graphics(st, rc - (st-input));
		printf("Graphics=1\n");
	      } else {
		write(1, input, rc);
	      }
	    } else {
	      char *st = strstr(input, END_GRAPH);
	      if (st != NULL) {
		st += 1;
		in_graph = 0;
		printf("Graphics=0\n");
		handle_graphics(input, st-input);
		write(1, st, rc - (st-input));
	      } else {
		handle_graphics(input, rc);
	      }
	    }
          } else if (rc < 0) {
	    perror("Error read master PTY");
	    exit(1);
          } else {
	    exit(0);
	  }
        }
      }
      } // End switch
    } // End while
  } else {
    struct termios slave_orig_term_settings; // Saved terminal settings
    struct termios new_term_settings; // Current terminal settings

    // CHILD

    // Close the master side of the PTY
    close(fdm);

    // Save the defaults parameters of the slave side of the PTY
    rc = tcgetattr(fds, &slave_orig_term_settings);

    // Set RAW mode on slave side of PTY
    new_term_settings = slave_orig_term_settings;
    cfmakeraw (&new_term_settings);
    tcsetattr (fds, TCSANOW, &new_term_settings);

    // The slave side of the PTY becomes the standard input and
    // outputs of the child process
    close(0); // Close standard input (current terminal)
    close(1); // Close standard output (current terminal)
    close(2); // Close standard error (current terminal)

    dup(fds); // PTY becomes standard input (0)
    dup(fds); // PTY becomes standard output (1)
    dup(fds); // PTY becomes standard error (2)

    // Now the original file descriptor is useless
    close(fds);

    // Make the current process a new session leader
    setsid();

    // As the child is a session leader, set the controlling terminal
    // to be the slave side of the PTY (Mandatory for programs like
    // the shell to make them manage correctly their outputs)
    ioctl(0, TIOCSCTTY, 1);

    // Execution of the program
    char **child_av;
    int i;

    // Build the command line
    child_av = (char **)malloc(ac * sizeof(char *));
    for (i = 1; i < ac; i ++) {
      child_av[i - 1] = strdup(av[i]);
    }
    child_av[i - 1] = NULL;
    rc = execvp(child_av[0], child_av);

    // if Error...
    return 1;
  }

  return 0;
} // main

