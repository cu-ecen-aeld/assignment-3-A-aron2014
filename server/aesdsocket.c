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
	sa.sa_flag = 0;
	
	if(sigaction(SIGINT, &sa, NULL)==-1) return -1;
	
	if(sigaction(SIGTERM, &sa, NULL)==-1) return -1;
	
	return 0;
}

static int open_listen_socket(void){

   if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        //fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
	syslog(LOG_ERROR, "getaddrinfo: %s\n", gai_strerror(rv));
        
	return -1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("server: socket");
	    syslog(LOG_ERROR, "server socket: %s", strerror(errno));
        
	    continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,sizeof(int)) == -1) {
            perror("setsockopt");
	    syslog(LOG_ERROR, "setsockopt: %s", strerror(errno));
	    freeaddrinfo(servinfo);//free if failed
	    close(sockfd);
            
	    return -1;
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
	    syslog(LOG_ERROR, "server bind: %s", strerror(errno));
            
	    continue;
        }

        break;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (p == NULL)  {
        //fprintf(stderr, "server: failed to bind\n");
	syslog(LOG_ERROR, "server: failed to bind\n");
	close(sockfd);
        
	return -1;
    }

    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
	syslog(LOG_ERROR, "listen: %s", strerror(errno));
	close(sockfd);
        
	return -1;
    }

    return sockfd;
	
}

static int send_file(int data_fd, int client_fd){
     char out[CHUNK_SIZE];

     if(lseek(data_fd, 0,SEEK_SET )!=0){
     	syslog(LOG_ERROR, "lseek: %s", strerror(errno));
     	
	return -1;	
     }
     ssize_t sent,n,off=0;
     for(;;)
     {
	     n=read(data_fd, out, sizeof out);
	     off = sizeof out;
	     if(n==0){
		syslog(LOG_DEBUG, "file sender: end of file reached.");
		return 0;
	     }
	     if(n<1){
		if(errno==EINTR)
			continue;
		syslog(LOG_ERROR, "read: %s", strerror(errno));
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

	size_t new_cap = (*cap==0)?CHUNK:*cap;
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

		while(len>0 &&(nl=memchar(buf, '\n', len))!=NULL){
			size_t pkt=(size_t)(nl-buf)+1; //this will include the \n character
			if (write(data_fd, buf, pkt)=(size_t)pkt){
				syslog(LOG_ERR, "write %s failed: %s", DATA_FILE, strerror(errno));
				ret=-1;
				free(buf);
				return ret;
			}
			if(send_file(data_fd, client_fd)==-1){
				syslog(LOG_ERR, "send_file: %s", strerror(errno));
				ret=-1;
				free(buf);
				return ret;
			memmove(buf,buf+pkt, len-pkt);//handle overlapping regions
						      //len-=pkt;
		}
	}
	free(buf);
	return ret;
}

int main(void)
{
    const char *filepath = DATAFILE;
    // listen on sock_fd, new connection on new_fd
    int sockfd, new_fd,write_fd;
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address info
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv, rc;
    ssize_t rec_stat;
    char *packet_buf;

    openlog("aesdsocket", LOG_PID, LOG_USER);

    while(!exit_requested) {  // main accept() loop
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr,
            &sin_size);
        if (new_fd == -1) {
		if(errno == EINTR) continue;
            	syslog(LOG_ERROR, "accept: %s", strerror(errno));
            	continue;
        }

        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);
       
	syslog(LOG_INFO, "Accepted connection from %s\n", s);
	
	rec_stat =recv(new_fd, packet_buf, sizeof(chunk), 0);
	if(rec_stat==-1){
		syslog(LOG_ERORR, "receive: %s", strerror(errno));
	}
	else if (rec_stat==0){
		syslog(LOG_WARNING, "remote client closed connection!");
	}
	memchar(packet_buf,

    }

    return 0;
}
