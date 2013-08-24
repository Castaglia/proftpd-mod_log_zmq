#!/usr/bin/env perl

use strict;

use ZMQ::Constants qw(ZMQ_PULL ZMQ_SUB ZMQ_SUBSCRIBE);
use ZMQ::LibZMQ3;

my $addr = $ARGV[0];
print STDOUT "binding socket to $addr\n";

my $ctx = zmq_init();

#my $sock_type = ZMQ_SUB;
my $sock_type = ZMQ_PULL;
my $sock = zmq_socket($ctx, $sock_type);
unless ($sock) {
  die("Can't create $sock_type ZMQ socket: $!");
}

if ($sock_type == ZMQ_SUB) {
  print STDOUT "setting SUBSCRIBE socket option to \"\"\n";
  zmq_setsockopt($sock, ZMQ_SUBSCRIBE, "");
}

if (zmq_connect($sock, $addr)) {
  die("Can't connect ZMQ socket to $addr: $!");
}

print STDOUT "waiting to receive message\n";
while (my $msg = zmq_recvmsg($sock)) {
  print STDOUT "received message:\n", zmq_msg_data($msg), "\n\n";
  print STDOUT "waiting to receive message\n";
}

exit 0;
