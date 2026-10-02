//Using the Beej Guide Server.c as base scaffolding for this

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h> //for inet_ntop
#include <netinet/in.h> //for sockarddr_in
#include <netdb.h>
#include <syslog.h>
#include <signal.h>
#include <fcntl.h>
#include <stdbool.h>

#define PORT ("9000")
#define BACKLOG (10)
#define DATAFILE ("/var/tmp/aesdsocketdata")
#define CHUNK_SIZE (1024)
// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

static volatile sig_atomic_t exit_requested = 0;
//Set flag to not interrupt current while loop cycle
static void handle_signal(int signo){
       	(void)signo;
        exit_requested = 1;
}

static int set_signals(void){
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	
	if(sigaction(SIGINT, &sa, NULL)==-1) return -1;
	
	if(sigaction(SIGTERM, &sa, NULL)==-1) return -1;
	
	return 0;
}

static int open_listen_socket(void){
	struct addrinfo hints, *servinfo, *p;
	int sockfd = -1;
	int yes = 1;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
   	hints.ai_flags    = AI_PASSIVE;
  	 if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        	//fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		syslog(LOG_ERR, "getaddrinfo: %s\n", gai_strerror(rv));
        
		return -1;
	 }

	// loop through all the results and bind to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
        	if ((sockfd = socket(p->ai_family, p->ai_socktype,p->ai_protocol)) == -1) {
            		perror("server: socket");
	    		syslog(LOG_ERR, "server socket: %s", strerror(errno));
        
	    		continue;
        	}

        	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,sizeof(int)) == -1) {
            		perror("setsockopt");
	    		syslog(LOG_ERR, "setsockopt: %s", strerror(errno));
	    		freeaddrinfo(servinfo);//free if failed
	    		close(sockfd);
            
	    		return -1;
        	}

        	if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            		close(sockfd);
            		perror("server: bind");
	    		syslog(LOG_ERR, "server bind: %s", strerror(errno));
            
	    		continue;
       		 }

        	break;
    	}

    	freeaddrinfo(servinfo); // all done with this structure

    	if (p == NULL)  {
        	//fprintf(stderr, "server: failed to bind\n");
		syslog(LOG_ERR, "server: failed to bind\n");
		close(sockfd);
        
		return -1;
    	}

    	if (listen(sockfd, BACKLOG) == -1) {
        	perror("listen");
		syslog(LOG_ERR, "listen: %s", strerror(errno));
		close(sockfd);
        
		return -1;
    	}

   	return sockfd;
	
}

static int send_file(int data_fd, int client_fd){
     char out[CHUNK_SIZE];

     if(lseek(data_fd, 0,SEEK_SET )<0){
     	syslog(LOG_ERR, "lseek: %s", strerror(errno));
     	
	return -1;	
     }
     ssize_t sent,n,off=0;
     for(;;)
     {
	     n=read(data_fd, out, sizeof out);
	     off=0;
	     if(n==0){
		syslog(LOG_DEBUG, "file sender: end of file reached.");
		return 0;
	     }
	     if(n<1){
		if(errno==EINTR)
			continue;
		syslog(LOG_ERR, "read: %s", strerror(errno));
		return -1;
	     }

	     while (off<n){
	     	sent =send(client_fd, out+off, n-off,MSG_NOSIGNAL);
		if(sent<0){
			if(errno==EINTR)
				continue;
			syslog(LOG_ERR,"send: %s", strerror(errno));

			return -1;
		}
	     	off+= sent;
	     }
     }	     
}

static int grow_buffer(char **buf, size_t *cap, size_t need){
	if (need<*cap)
		return 0;

	size_t new_cap = (*cap==0)?CHUNK_SIZE:*cap;
	//Grow the size of the new max size for the buffer
	while(new_cap<need)
		new_cap*=2;
	//dynamically reallocate buffer size to new max size
	char *tmp = realloc(*buf, new_cap);
	if (tmp==NULL)
		return -1;
	*buf=tmp;
	*cap=new_cap;
	return 0;
}
static int handle_client(int client_fd, int data_fd){
	char	chunk[CHUNK_SIZE];
	char	*buf=NULL;
	size_t	len=0;
	size_t	cap=0;
	int	ret=0;
	bool discarding=false;
	
	for(;;){
		ssize_t n =recv(client_fd, chunk, sizeof chunk, 0);
		if(n==0)
			break;//Client closed
		if(n<0){
			if(errno==EINTR){
				if(exit_requested)
					break;//clean stop for SIGTERM/SIGINT
				continue;
			}
			syslog(LOG_ERR, "recv: %s", strerror(errno));
			ret=-1;
			break;
		}
		char *in =chunk;
		size_t in_len=(size_t)n;

		while(in_len>0){
			if(discarding){
				//Handling for over-length packet
				char *nextline = memchr(in,'\n', in_len);
				if(nextline==NULL){
					in_len=0;//whole chunk missed a newline character
				}
				else{
					size_t skip=(size_t)(nextline-in)+1;
					in += skip;
					in_len -= skip;
					discarding=false;
				}
				continue;
			}
			if(grow_buffer(&buf, &cap, len+in_len)==-1){
				syslog(LOG_ERR, "out of memory at %zu bytes, discarding packet", len+in_len);
				len = 0;
				discarding = true;
				continue;
			}
			memcpy(buf+len, in, in_len);
			len+=in_len;
			in_len=0;
		}

		//Handle complete packets
		char *nl;

		while(len>0 &&(nl=memchr(buf, '\n', len))!=NULL){
			size_t pkt=(size_t)(nl-buf)+1; //this will include the \n character
			if (write(data_fd, buf, pkt)!=(size_t)pkt){
				syslog(LOG_ERR, "write %s failed: %s", DATAFILE, strerror(errno));
				ret=-1;
				free(buf);
				return ret;
			}
			if(send_file(data_fd, client_fd)==-1){
				syslog(LOG_ERR, "send_file: %s", strerror(errno));
				ret=-1;
				free(buf);
				return ret;
			}
			memmove(buf,buf+pkt, len-pkt);//handle overlapping regions
			len-=pkt;
		}
	}
	free(buf);
	return ret;
}

static int run_daemon(void){
	pid_t pid = fork();
	if(pid<0){
		syslog(LOG_ERR,"fork: %s", strerror(errno));
		return -1;
	}
	if (pid>0){
		_exit(0);
	}
	if(setsid()==-1){
		syslog(LOG_ERR, "setsid: %s", strerror(errno));
		return -1;
	}
	if(chdir("/")==-1){
		syslog(LOG_ERR, "chdir: %s", strerror(errno));
		return -1;
	}

	int devnull = open("/dev/null", O_RDWR);
	if(devnull!=-1){
		dup2(devnull, STDIN_FILENO);
		dup2(devnull, STDOUT_FILENO);
		dup2(devnull, STDERR_FILENO);
		if (devnull>STDERR_FILENO){
			close(devnull);
		}
	}
	return 0;
}
int main(int argc, char *argv[])
{
	int ret = -1;
 	int listen_fd = -1;
 	int data_fd = -1;
	
	bool start_daemon=(argc>=2 && strcmp(argv[1], "-d")==0);

	openlog("aesdsocket",LOG_PID, LOG_USER);

	if(set_signals()==-1){
		syslog(LOG_ERR, "sigaction: %s", strerror(errno));
	    	goto errorhandler;
    	}
    	listen_fd=open_listen_socket();

   	if (listen_fd==-1)
		goto errorhandler;

	if (start_daemon && run_daemon()==-1)
		goto errorhandler;
    	data_fd=open(DATAFILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    	if(data_fd==-1){
		syslog(LOG_ERR, "open %s: %s", DATAFILE, strerror(errno));
	    	goto errorhandler;
   	}

    	while(!exit_requested){
		struct sockaddr_storage client_addr;
		socklen_t sin_size= sizeof client_addr;
		char s[INET6_ADDRSTRLEN];
		
		int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size);
		if (new_fd==-1){
			if(errno==EINTR)
				continue;
			syslog(LOG_ERR,"accept: %s", strerror(errno));
			continue;
		}

		inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), s, sizeof s);
		syslog(LOG_INFO, "Accepted connection from %s", s);
		handle_client(new_fd, data_fd);
		close(new_fd);
		syslog(LOG_INFO, "Closed connection from %s", s);
	}

	syslog(LOG_INFO, "Caught signal, exiting");

	ret= 0;

errorhandler:
	if(data_fd!=-1)
		close(data_fd);
	if(listen_fd!=-1)
		close(listen_fd);
	unlink(DATAFILE);
	closelog();
	return ret;
}
