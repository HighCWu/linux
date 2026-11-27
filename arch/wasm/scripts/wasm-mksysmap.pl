#!/usr/bin/env perl
# SPDX-License-Identifier: GPL-2.0-only
use strict;
use warnings;

my ($map_file) = @ARGV;
die "usage: $0 <wasm-ld.map> < <vmlinux.wat>\n" unless defined $map_file;

my %table_idx;
my %func_idx;
my $next_func = 0;

while (<STDIN>) {
    if (/^\s*\(import\b.*?\(func(?:\s+\$([^\s()]+))?/) {
        $next_func++;
        next;
    }
    if (/^\s*\(func(?:\s+\$([^\s()]+))?/) {
        if (defined $1) {
            my $name = $1;
            $func_idx{$name} = $next_func unless exists $func_idx{$name};
        }
        $next_func++;
        next;
    }
    if (/^\s*\(elem\b.*?\((?:i32|i64)\.const\s+(\d+)\)\s+func\s+(.+)\)\s*$/) {
        my $idx = int($1);
        for my $tok (split /\s+/, $2) {
            if ($tok =~ /^\$(.+)$/) {
                $table_idx{$1} = $idx unless exists $table_idx{$1};
            }
            $idx++;
        }
    }
}

my @syms;
my %seen;

open my $mh, '<', $map_file or die "open $map_file: $!\n";
my $section = '';
while (<$mh>) {
    chomp;
    if (/^\s*-\s+\S+\s+\S+\s+(\S+)\s*$/) {
        $section = $1;
        next;
    }
    if ($section eq 'DATA' && /^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s{16,}(\S+)\s*$/) {
        my ($addr_hex, $name) = ($1, $2);
        next if $name =~ /^(?:\.|\$)/;
        push @syms, [ hex($addr_hex), 'D', $name ];
        $seen{$name} = 1;
    }
}
close $mh;

for my $name (sort keys %table_idx) {
    next if $name =~ /^(?:__wasm_|\.)/;
    push @syms, [ $table_idx{$name}, 'T', $name ];
    $seen{$name} = 1;
}

for my $name (sort keys %func_idx) {
    next if $seen{$name} || $name =~ /^(?:__wasm_|\.)/;
    push @syms, [ 0x80000000 + $func_idx{$name}, 't', $name ];
}

@syms = sort { $a->[0] <=> $b->[0] || $a->[2] cmp $b->[2] } @syms;
for my $s (@syms) {
    printf "%016x %s %s\n", $s->[0], $s->[1], $s->[2];
}
