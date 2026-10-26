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
#include <stdatomic.h>
#include <sys/queue.h> 
#include <pthread.h>
#include <semaphore.h>
#include <sys/time.h>
#include <time.h>

#include <sys/ioctl.h>
#include "../aesd-char-driver/aesd_ioctl.h"

#define SEEKTO_START "AESDCHAR_IOCSEEKTO:"
#define SEEKTO_LEN (sizeof(SEEKTO_START)-1)

#ifndef USE_AESD_CHAR_DEVICE
#define USE_AESD_CHAR_DEVICE 1
#endif

#define PORT ("9000")
#define BACKLOG (10)// how many pending connections queue will hold
#if USE_AESD_CHAR_DEVICE
#define DATAFILE ("/dev/aesdchar")
#else
#define DATAFILE "/var/tmp/aesdsocketdata"
#endif
#define CHUNK_SIZE (1024)//set chunk size to 1KB for reading and writing data

//Thread parameters 
struct thread_node{
	pthread_t socket_thread;
	atomic_bool is_complete;
	int client_fd;
	int data_fd;
	pthread_mutex_t *file_mtx;//point to mutex in main
	char client_ip[INET6_ADDRSTRLEN]; //for thread to log at socket close
	SLIST_ENTRY(thread_node) entries;

};

SLIST_HEAD(thread_list, thread_node);

static sem_t timed_sem;

static atomic_int exit_requested = 0;
//Set flag to not interrupt current while loop cycle
static void handle_signal(int signo){
	(void)signo;
	exit_requested = 1;
}

//Signal handler for SIGALRM to post to semaphore
static void handle_alarm(int signo){
	(void)signo;
	int saved_errno=errno;//store errno temporarily in case an interrupt writes errno
	sem_post(&timed_sem);
	errno=saved_errno;
}

//set up signal mask to block SIGINT, SIGTERM, and SIGALRM for a thread.
static void block_signals(sigset_t *old){
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &set,old);
}

struct ts_args{
	int data_fd;
	pthread_mutex_t *file_mtx;
};
//ignore this if using aesd char device
#if !USE_AESD_CHAR_DEVICE
//Thread function to handle timestamp calls every 10s, called by SIGALRM and posts to semaphore to wake up this thread
static void *timestamp_thread(void *arg){
	struct ts_args *timestamp = arg;
	char line[128];

	while(1){
		if(sem_wait(&timed_sem)==-1){
			if(errno==EINTR){
				continue;
			}
			syslog(LOG_ERR, "sem_wait:%s",strerror(errno));
			break;
		}
		if(exit_requested) //posted from main to wake up
			break;
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm tm;
		if(localtime_r(&ts.tv_sec, &tm)==NULL){
			continue;
		}
		size_t len = strftime(line, sizeof line, "timestamp:%a, %d %b %Y %H:%M:%S %z\n", &tm);
		if(len==0)
			continue;

		pthread_mutex_lock(timestamp->file_mtx);
		ssize_t w = write(timestamp->data_fd,line,len);
		pthread_mutex_unlock(timestamp->file_mtx);
		if(w!=(ssize_t)len)
			syslog(LOG_ERR,"timestamp write: %s", strerror(errno));

	}
	return NULL;
}
#endif
// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

//Set up signal handlers for SIGINT and SIGTERM to set exit_requested flag
static int set_signals(void){
	struct sigaction sa;
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	
	if(sigaction(SIGINT, &sa, NULL)==-1) return -1;
	
	if(sigaction(SIGTERM, &sa, NULL)==-1) return -1;
	
	struct sigaction sa_alrm;
	memset(&sa_alrm,0,sizeof sa_alrm);
	sa_alrm.sa_handler=handle_alarm;
	sa_alrm.sa_flags=0;
	if(sigaction(SIGALRM,&sa_alrm,NULL)==-1)
		return -1;
	return 0;
}

//Open and bind listen socket for incoming connections
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
		//Allow socket to be reused immediately after program exits
        	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,sizeof(int)) == -1) {
            		perror("setsockopt");
	    		syslog(LOG_ERR, "setsockopt: %s", strerror(errno));
	    		freeaddrinfo(servinfo);//free if failed
	    		close(sockfd);
            
	    		return -1;
        	}
		//bind socket to port
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
	//Listen for incoming connections
    	if (listen(sockfd, BACKLOG) == -1) {
        	perror("listen");
		syslog(LOG_ERR, "listen: %s", strerror(errno));
		close(sockfd);
        
		return -1;
    	}

   	return sockfd;
}

//Send file contents to client socket
static int send_file(int data_fd, int client_fd){
	//set output buffer to 1KB for reading and writing data
     char out[CHUNK_SIZE];
    
     ssize_t sent,n,off=0;
         //Loop to read from file and send to client socket until end of file is reached
     while(1)
     {
	     n=read(data_fd, out, sizeof out);
	     off=0;
	     if(n==0){
		syslog(LOG_DEBUG, "file sender: end of file reached.");
		return 0;
	     }
	     if(n<1){
		syslog(LOG_ERR, "read: %s", strerror(errno));
		return -1;
	     }
	     //Loop to send data to client socket until all bytes are sent
	     while (off<n){
		//MSG_NOSIGNAL flag is used to prevent SIGPIPE signal from being sent to the process if the client has closed the connection.
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


#if USE_AESD_CHAR_DEVICE
static bool is_seek_command(const char *buffer, size_t pkt){
	return pkt > SEEKTO_LEN && strncmp(buffer,SEEKTO_START, SEEKTO_LEN)==0; //using strncmp to compare both strings up to SEEKTO_LEN characters
}

static int parse_seek(struct thread_node *node, const char *buffer){
	struct aesd_seekto seekto;
	unsigned int cmd, offset;
	//validate that 2 commands were provided
	if(sscanf(buffer+SEEKTO_LEN, "%u,%u", &cmd, &offset)!=2){
		syslog(LOG_ERR, "parse_seek: sscanf failed for %s", buffer);
		return -1;
	}
	seekto.write_cmd=cmd;
	seekto.write_cmd_offset=offset; 

	int fd = open(DATAFILE, O_RDWR);
	if(fd==-1){
		syslog(LOG_ERR, "open %s: %s", DATAFILE, strerror(errno));
		return -1;
	}

	if(ioctl(fd,AESDCHAR_IOCSEEKTO, &seekto)==-1){
		syslog(LOG_ERR,"ioctl: %s", strerror(errno));
		close(fd);
		return -1;
	}

	int rc =send_file(fd,node->client_fd);
	close(fd);
	return rc;
}

#endif

//Adding packet writer for AESDCHAR_DEVICE to write to the char device instead of a file
static int write_packet(struct thread_node *node, const char *buf, size_t pkt){
#if USE_AESD_CHAR_DEVICE
	(void)node;

	int fd = open(DATAFILE, O_WRONLY);
	if(fd==-1){
		syslog(LOG_ERR, "open %s: %s", DATAFILE, strerror(errno));
		return -1;
	}
	ssize_t w=write(fd,buf,pkt);
	close(fd);
	return(w==(ssize_t)pkt)?0:-1;
#else
	return (write(node->data_fd,buf,pkt)==(ssize_t)pkt)?0:-1;
#endif
}

//Reading from the CHAR device or file and sending data back to the client socket
static int send_data(struct thread_node *node){
#if USE_AESD_CHAR_DEVICE
	int fd=open(DATAFILE,O_RDONLY);
	if(fd==-1){
		syslog(LOG_ERR, "open %s: %s", DATAFILE, strerror(errno));
		return -1;
	}
	int rc=send_file(fd,node->client_fd);
	close(fd);
	return rc;
#else
	//Reset file offset to beginning of file before sending data back to client socket
	if(lseek(node->data_fd,0,SEEK_SET)<0){
		syslog(LOG_ERR, "lseek: %s", strerror(errno));
		return -1;
	}
	return send_file(node->data_fd, node->client_fd);
#endif
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

//Handle client connection, read data from socket, write to file, and send file contents back to client
static int handle_client(struct thread_node *node){
	char	chunk[CHUNK_SIZE];
	char	*buf=NULL;
	size_t	len=0;
	size_t	cap=0;
	int	ret=0;
	bool discarding=false;
	
	while(1){
		//Read data from client socket into chunk buffer
		ssize_t n =recv(node->client_fd, chunk, sizeof chunk, 0);
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
			//break;
			goto handle_exit;
		}
		char *in =chunk;
		size_t in_len=(size_t)n;

		//Loop to process incoming data from client socket, handle over-length packets, and write complete packets to file
		while(in_len>0){
			//Check if currently discarding data due to over-length packet
			if(discarding){
				//Handling for over-length packet
				char *nextline = memchr(in,'\n', in_len);
				if(nextline==NULL){
					in_len=0;//whole chunk missed a newline character
				}
				else{
					//Found newline character, stop discarding and continue processing
					size_t skip=(size_t)(nextline-in)+1;
					in += skip;
					in_len -= skip;
					discarding=false;
				}
				continue;
			}
			//Check if incoming data exceeds maximum packet size, and if so, discard the packet and log an error message
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
		//Lock only here since this is where globabl data is handled
		pthread_mutex_lock(node->file_mtx);
		while(len>0 &&(nl=memchr(buf, '\n', len))!=NULL){
			size_t pkt=(size_t)(nl-buf)+1; //this will include the \n character

			if(is_seek_command(buf,pkt)){
				if(parse_seek(node,buf)==-1){
					ret = -1;
					pthread_mutex_unlock(node->file_mtx);

					goto handle_exit;
				}
			}
			else{
				if (write_packet(node, buf, pkt) ==-1){
					syslog(LOG_ERR, "write %s failed: %s", DATAFILE, strerror(errno));
					ret=-1;
					pthread_mutex_unlock(node->file_mtx);
					
					goto handle_exit;
				}
				//Send the contents of the file back to the client socket
				if(send_data(node)==-1){
					syslog(LOG_ERR, "send_file: %s", strerror(errno));
					ret=-1;
					pthread_mutex_unlock(node->file_mtx);
					
					goto handle_exit;
				}
			}
			memmove(buf,buf+pkt, len-pkt);//handle overlapping regions
			len-=pkt;

		}
		//Unlock the mutex after processing the data as a catchall.
		pthread_mutex_unlock(node->file_mtx);
	}
	handle_exit:
		free(buf);

		return ret;
}

//Thread function to handle client connection, called by main thread
static void *thread_handler(void *arg){
	struct thread_node *node=arg;
	handle_client(node);
	close(node->client_fd);
	syslog(LOG_INFO, "Connection closed from %s", node->client_ip);
	atomic_store(&node->is_complete, true);
	return NULL;
}

//Run the program as a daemon by forking a child process, 
//creating a new session, changing the working directory to root, 
// and redirecting standard input/output/error to /dev/null
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
	//Redirect standard input/output/error to /dev/null
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
	//Check if the -d option is specified to run the program as a daemon
	bool start_daemon=(argc>=2 && strcmp(argv[1], "-d")==0);
	//Open syslog for logging
	openlog("aesdsocket",LOG_PID, LOG_USER);
	//Set up signal handlers for SIGINT and SIGTERM to set exit_requested flag
	if(set_signals()==-1){
		syslog(LOG_ERR, "sigaction: %s", strerror(errno));
	    	goto cleanup;
    	}
	//Open and bind listen socket for incoming connections
    	listen_fd=open_listen_socket();

   	if (listen_fd==-1)
		goto cleanup;
	//If -d option is specified, run listening socket as daemon
	if (start_daemon && run_daemon()==-1)
		goto cleanup;
#if !USE_AESD_CHAR_DEVICE
    	data_fd=open(DATAFILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    	if(data_fd==-1){
		syslog(LOG_ERR, "open %s: %s", DATAFILE, strerror(errno));
	    	goto cleanup;
   	}
#endif

	struct thread_list head;
	SLIST_INIT(&head); //Initialize the queue
	pthread_mutex_t socket_mtx = PTHREAD_MUTEX_INITIALIZER;
	
	sigset_t old;
#if !USE_AESD_CHAR_DEVICE
	pthread_t ts_thread;	
	//Set up timestamp logic and initialize semaphore
	if(sem_init(&timed_sem,0,0)==-1){
		syslog(LOG_ERR,"sem_init: %s", strerror(errno));
		goto cleanup;
	}
	struct ts_args ts = {.data_fd =data_fd, .file_mtx=&socket_mtx};
	struct itimerval it = {.it_interval={10,0}, .it_value={10,0}};
	//Block signals for main thread and spawn timestamp thread
    	block_signals(&old);
	
	int ts_rc = pthread_create(&ts_thread,NULL,timestamp_thread,&ts);
	//restore original signal mask for main thread so that SIGINT and SIGTERM can be handled
	pthread_sigmask(SIG_SETMASK,&old,NULL);
	//Handle return code for thread creation and release semaphore.
	if(ts_rc!=0){
		syslog(LOG_ERR, "pthread_create (timestamp): %s", strerror(ts_rc));
		sem_destroy(&timed_sem);
		goto cleanup;
	}
	//Set up timer to send SIGALRM every 10 seconds
	if(setitimer(ITIMER_REAL,&it,NULL)==-1){
		syslog(LOG_ERR,"setitimer: %s", strerror(errno));
	}
#endif
	//Main loop to accept incoming connections and spawn threads to handle them
	while(!exit_requested){
		struct sockaddr_storage client_addr;
		socklen_t sin_size= sizeof client_addr;
		char s[INET6_ADDRSTRLEN];
	
		//Check here for if any threads in the linked list are complete
		struct thread_node *prev=NULL;
		struct thread_node *n=SLIST_FIRST(&head);
		while(n!=NULL){
			struct thread_node *next= SLIST_NEXT(n,entries);
			if(atomic_load(&n->is_complete)){
				pthread_join(n->socket_thread,NULL);
				if(prev==NULL){
					SLIST_REMOVE_HEAD(&head,entries);
				}
				else{
					SLIST_NEXT(prev,entries)=next;
				}
				free(n);
			}
			else{
				prev=n;
			}
			n=next;
		}
		//Accept incoming connection
		int new_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &sin_size);
		if (new_fd==-1){
			if(errno==EINTR)
				continue;
			syslog(LOG_ERR,"accept: %s", strerror(errno));
			continue;
		}
		//Get the IP address of the client and log it
		inet_ntop(client_addr.ss_family, get_in_addr((struct sockaddr *)&client_addr), s, sizeof s);
		syslog(LOG_INFO, "Accepted connection from %s", s);
		syslog(LOG_INFO, "Spawning thread for fd %d", new_fd);
			
		//Add new thread to linked list, using calloc to allocate zeroed  memory for the thread node 
		struct thread_node *node = calloc(1, sizeof *node);
		if (node==NULL){
			syslog(LOG_ERR, "calloc failed, terminating conenction");
			close(new_fd);
			continue;
		}
		//Initialize thread node and spawn thread to handle client connection
		node->client_fd=new_fd;
		node->data_fd=data_fd;
		node->file_mtx=&socket_mtx;
		memcpy(node->client_ip, s, sizeof node->client_ip);
		//Block signals for thread creation to avoid race conditions with signal handling
		block_signals(&old);
		int rc = pthread_create(&node->socket_thread, NULL, thread_handler, node);
		pthread_sigmask(SIG_SETMASK, &old,NULL);

		if(rc!=0){
			//failed to create thread, close socket and free data
			syslog(LOG_ERR, "pthread_create: %s", strerror(rc));
			close(new_fd);
			free(node);
			continue;

		}
		//Add thread node to linked list
		SLIST_INSERT_HEAD(&head, node, entries);
	}

	syslog(LOG_INFO, "Caught signal, exiting");

	ret= 0;
#if !USE_AESD_CHAR_DEVICE
	//Clean up resources and exit gracefully
	struct itimerval off={0};
	setitimer(ITIMER_REAL, &off, NULL);
#endif
	close(listen_fd);
	listen_fd=-1;
	//Close all client connections and join threads
	struct thread_node *n;
	SLIST_FOREACH(n,&head,entries)
		shutdown(n->client_fd, SHUT_RDWR);
	while(!SLIST_EMPTY(&head)){
		n=SLIST_FIRST(&head);
		pthread_join(n->socket_thread,NULL);
		SLIST_REMOVE_HEAD(&head, entries);
		free(n);
	}
#if !USE_AESD_CHAR_DEVICE
	//Post to semaphore to wake up timestamp thread and join it
	sem_post(&timed_sem);
	pthread_join(ts_thread,NULL);
	sem_destroy(&timed_sem);
#endif
	pthread_mutex_destroy(&socket_mtx);
cleanup:
	if(data_fd!=-1)
		close(data_fd);
	if(listen_fd!=-1)
		close(listen_fd);
#if !USE_AESD_CHAR_DEVICE
	unlink(DATAFILE);
#endif
	closelog();
	return ret;
}
