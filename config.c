#include "config.h"
#include "socket.h"
#include "frame.h"

#include <stdio.h>
#include <netdb.h>
#include <stdlib.h>
#include <strings.h>

#define PORT_RES_MAX_ATTEMPTS 3
#define SIZEOF_NEG_T(num_ports) (sizeof(neg_t) + (num_ports-1)*sizeof(unsigned short))

typedef enum {
	ACK = 0,
	NACK = 1,
	REJ = 2
} ack_t;

typedef struct {
	ack_t ack;
	blen_t blen;
	unsigned short streams;
	unsigned short port[];
} neg_t;

long random_minmax(long min, long max)
{
	max = max - min; // get actual max for random number generation

	unsigned long num_bins = (unsigned long)max + 1;
	unsigned long num_rand = (unsigned long)RAND_MAX + 1;
	unsigned long bin_size = num_rand / num_bins;
	unsigned long defect   = num_rand % num_bins;

	long x;
	do
	{
		x = random();
	}
	while(num_rand - defect <= (unsigned long)x);

	return min + (x/bin_size); // offset min by random number
}

int reserve_ports(neg_t *opt, sock_list_t *socks)
{
	/* attempt to reserve as many ports as requested */
	int attempts = 0;
	while(socks->len < opt->streams && attempts < PORT_RES_MAX_ATTEMPTS)
	{
		/* choose a new random port number that's not already in the list */
		unsigned short new_port;
		while(1)
		{
			new_port = random_minmax(40000,65000);

			char exists = 0;
			for(size_t i=0; i<opt->streams; ++i)
			if(new_port == opt->port[i]) exists = 1;

			if(exists == 0) break;
		}

		/* reserve the port */
		int new_sock = sock_listen(new_port);
		if(new_sock == -1) ++attempts; // failed - don't store
		else // success, store
		{
			attempts = 0;

			/* reserve memory */
			if(socks->len == 0)
			{
				socks->sock = malloc(sizeof(*socks->sock));
				if(socks->sock == 0) return -1;
			}
			else
			{
				void *ptr = realloc(socks->sock,(socks->len+1)*sizeof(*socks->sock));
				if(ptr == 0) return -1;
				else socks->sock = ptr;
			}

			/* record port and socket */
			socks->sock[socks->len] = new_sock;
			opt->port[socks->len++] = new_port;
		}
	}

	/* validate number of ports reserved */
	if(socks->len < opt->streams) // less than requested
	{
		opt->streams = socks->len;
		return -1;
	}
	else // as requested
		return 0;
}

int configure_send(int argc, char *argv[], conf_t *conf)
{
	/* configure negotiation connection */
	unsigned short cport = atoi(argv[argc-1]);
	unsigned long caddr;
	{
		struct hostent *server = gethostbyname(argv[argc-2]);
		if(server == NULL)
		{
			fprintf(stderr,"Cannot send: Unknown host");
			exit(0);
		}
		bcopy((char *)server->h_addr,(char *) &caddr,server->h_length);
		caddr = ntohl(caddr);
	}
	fprintf(stderr,"Connecting to server...");
	int csock = sock_connect(caddr, cport);
	if(csock == -1)
	{
		fprintf(stderr," failed!: exiting.\n");
		exit(0);
	}
	else fprintf(stderr," done!\n");

	fbuf_t fbuf; // initialise frame buffer
	fbuf_init(&fbuf,csock,sizeof(blk_t) + BLEN_DEFAULT);


	/* negotiate connection options */
	neg_t *opt = malloc(SIZEOF_NEG_T(NUM_PORTS_MAX)); // initialize option structure
	opt->ack = NACK;
	opt->blen = BLEN_DEFAULT;
	opt->streams = NUM_PORTS_DEFAULT;
	opt->port[0] = 0;

	fprintf(stderr,"Sending configuration request...");
	put_frame(&fbuf,(char *)opt,SIZEOF_NEG_T(0)); // send configuration request
	fprintf(stderr," done!\n");

	while(opt->ack != ACK) // negotiate until configuration accepted
	{
		fprintf(stderr,"Waiting for configuration response...");
		get_frame(&fbuf,(char *)opt,SIZEOF_NEG_T(NUM_PORTS_MAX)); // wait for configuration response
		fprintf(stderr," received!\n");

		switch(opt->ack)
		{
		case ACK:
			fprintf(stderr,"Server signalled 'ACK': configuration successful!\n");
			break;
		case NACK:
			// do something to check amended option and send configuration response
			fprintf(stderr,"Cannot send: Server rejected configuration options with amendment.\n");
			exit(0);
			//break;

		case REJ:
			fprintf(stderr,"Cannot send: Server rejected configuration options without amendment.\n");
			exit(0);
		}
	}


	/* connect sockets */
	fprintf(stderr,"Connecting to sockets on ports");
	conf->socks.sock = malloc(opt->streams * sizeof(*conf->socks.sock));
	for(size_t n=0; n<opt->streams; ++n)
	{
		fprintf(stderr," %d",opt->port[n]);
		conf->socks.sock[n] = sock_connect(caddr,opt->port[n]);
		if(conf->socks.sock[n] == -1) fprintf(stderr,"(e!)");
	}
	conf->socks.len = opt->streams;
	fprintf(stderr,"... done!\n");

	return 0;
}

int configure_recv(int argc, char *argv[], conf_t *conf)
{
	/* configure negotiation connection */
	unsigned short cport = atoi(argv[argc-1]);
	fprintf(stderr,"Opening port...");
	int csock = sock_listen(cport);
	if(csock == -1)
	{
		fprintf(stderr," failed!: exiting.\n");
		exit(0);
	}
	else fprintf(stderr," done!\n");
	fprintf(stderr,"Waiting for client...");
	csock = sock_accept(csock);
	if(csock == -1)
	{
		fprintf(stderr," failed!: exiting.\n");
		exit(0);
	}
	else fprintf(stderr," done!\n");

	fprintf(stderr,"Initialising frame buffer...");
	fbuf_t fbuf; // initialise frame buffer
	int rc = fbuf_init(&fbuf,csock,sizeof(blk_t) + BLEN_DEFAULT);
	if(rc < 0)
	{
		fprintf(stderr," failed!: exiting.\n");
		exit(0);
	}
	else fprintf(stderr," done!\n");


	/* negotiate connection options */
	conf->socks.len = 0; // initialize sock list

	neg_t *opt = malloc(SIZEOF_NEG_T(NUM_PORTS_MAX)); // initialize option structure
	opt->ack = NACK;

	while(opt->ack != ACK) // negotiate until configuration accepted
	{
		fprintf(stderr,"Waiting for configuration response...");
		rc = get_frame(&fbuf,(char *)opt,SIZEOF_NEG_T(NUM_PORTS_MAX)); // wait for configuration response
		if(rc <= 0) { fprintf(stderr," error!: exiting.\n"); exit(0); }
		fprintf(stderr," received!\n");

		/* examine response */
		switch(opt->ack)
		{
		case NACK:
			fprintf(stderr,"Client signalled 'NACK': configuring...\n");

			opt->ack = ACK; // assume correct configuration - change flag to NACK if error

			/* check requested blen */
			// do nothing yet

			/* attempt to reserve ports */
			fprintf(stderr,"Reserving (%u) ports...",opt->streams);
			rc = reserve_ports(opt,&conf->socks);
			if(rc < 0) // verify number of reserved ports
			{
				fprintf(stderr," failed! (%u)\n",opt->streams);
				opt->ack = NACK;
			}
			else fprintf(stderr," done!\n");

			/* transmit reponse */
			switch(opt->ack)
			{
			case ACK:
				fprintf(stderr,"Configuration succeeded! Transmitting 'ACK'.\n");
				put_frame(&fbuf,(char *)opt,SIZEOF_NEG_T(opt->streams));
				break;
			case NACK:
				fprintf(stderr,"Configuration failed! Transmitting 'NACK'.\n");
				put_frame(&fbuf,(char *)opt,SIZEOF_NEG_T(opt->streams));
				break;
			case REJ:
				fprintf(stderr,"Configuration failed!: Transmitting 'REJ' and exiting.\n");
				put_frame(&fbuf,(char *)opt,SIZEOF_NEG_T(opt->streams));
				exit(0);
			}
			break;

		case ACK:

		case REJ:
			fprintf(stderr,"Cannot send: Client rejected configuration options without amendment.");
			exit(0);
		}
	}

	/* complete socket connections */
	fprintf(stderr,"Waiting for socket connections...");
	for(size_t n=0; n<conf->socks.len; ++n)
	{
		conf->socks.sock[n] = sock_accept(conf->socks.sock[n]);
	}
	fprintf(stderr,"... done!\n");

	return 0;
}
