#!/usr/bin/perl
use strict;
use warnings;

my $prev_empty = 0;

while (my $line = <>) {
    chomp $line;
    
    # Check if current line is empty (no text at all, not even whitespace)
    if ($line eq '') {
        if (!$prev_empty) {
            # First empty line in a sequence - add \newline
            print "\\newline\n";
            $prev_empty = 1;
        }
    } else {
    
	    # Reset empty line tracker
	    $prev_empty = 0;
    }
    
    # Rule 4: Message-Id pattern at the beginning of the line
    if ($line =~ /^Message-Id: <([^>]*)>/) {
        my $msg_id = $1;
        $line =~ s/^Message-Id: <[^>]*>/Message-Id: <\\nolinkurl{$msg_id}>/;
        # Message-Id also matches the header pattern, so add \newline
    }
    # Rule 1: Lines starting with Header-Name: pattern
    if ($line =~ /^[A-Z][A-Za-z-]*:/) {
        print "$line\\newline\n";
        #do not force empty lines after trailers
	$prev_empty = 1;
    } 
    else {
        print "$line\n";
    }
}
