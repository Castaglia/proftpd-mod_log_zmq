#!/usr/bin/env perl

use strict;

use ZMQ::Constants qw(ZMQ_PULL ZMQ_SUB ZMQ_SUBSCRIBE);
use ZMQ::LibZMQ3;

my $addr = $ARGV[0];

my $ctx = zmq_init();

#my $sock_type = ZMQ_SUB;
my $sock_type = ZMQ_PULL;

my $bind_socket = 1;
my $connect_socket = 2;
my $socket_mode = $connect_socket;

my $sock = zmq_socket($ctx, $sock_type);
unless ($sock) {
  die("Can't create $sock_type ZMQ socket: $!");
}

if ($sock_type == ZMQ_SUB) {
  print STDOUT "setting SUBSCRIBE socket option to \"\"\n";
  zmq_setsockopt($sock, ZMQ_SUBSCRIBE, "");
}

if ($socket_mode == $bind_socket) {
  print STDOUT "binding socket to $addr\n";

  if (zmq_bind($sock, $addr)) {
    die("Can't bind ZMQ socket to $addr: $!");
  }

} elsif ($socket_mode == $connect_socket) {
  print STDOUT "connecting socket to $addr\n";

  if (zmq_connect($sock, $addr)) {
    die("Can't connect ZMQ socket to $addr: $!");
  }
}

print STDOUT "waiting to receive message\n";
while (my $msg = zmq_recvmsg($sock)) {
  print STDOUT "received message:\n", zmq_msg_data($msg), "\n\n";
  print STDOUT "waiting to receive message\n";
}

exit 0;
