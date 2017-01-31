#!/usr/bin/perl

use warnings;
use strict;

use File::Basename;
use Getopt::Long;
use JSON::XS qw(decode_json);

my $CASING;
my %IMPORTS;

my %TYPE_MAP = (
    'char'               => 'char',
    'signed-char'        => 'char',
    'unsigned-char'      => 'uint8',
    'int'                => 'int',
    'signed-int'         => 'int',
    'unsigned-int'       => 'uint',
    'short'              => '(short-type)',
    'signed-short'       => '(short-type)',
    'unsigned-short'     => '(ushort-type)',
    'long'               => '(long-type)',
    'signed-long'        => '(long-type)',
    'unsigned-long'      => '(ulong-type)',
    'long-long'          => '(long-long-type)',
    'signed-long-long'   => '(long-long-type)',
    'unsigned-long-long' => '(ulong-long-type)',
    'ptrdiff_t'          => 'ptrdiff',
    'size_t'             => 'size',
    'uint8_t'            => 'uint8',
    'uint16_t'           => 'uint16',
    'uint32_t'           => 'uint32',
    'uint64_t'           => 'uint64',
    'int8_t'             => 'int8',
    'int16_t'            => 'int16',
    'int32_t'            => 'int32',
    'int64_t'            => 'int64',
);

my %CASING_MAP = (
    none     => \&casing_none,
    standard => \&casing_standard,
    camel    => \&casing_camel,
    lisp     => \&casing_lisp,
);

sub type_to_string
{
    my ($type) = @_;

    our $in_function;

    my $tag = $type->{'tag'};
    $tag =~ s/^://;

    if ($tag eq 'pointer') {
        return "(p ".(type_to_string($type->{'type'})).")";
    }
    if ($tag eq 'array') {
        if ($in_function) {
            return "(p ".(type_to_string($type->{'type'})).")";
        } else {
            return "(array-of ".$type->{'size'}." ".
                   (type_to_string($type->{'type'})).")";
        }
    }
    if ($tag eq 'bitfield') {
        my $bf_type = type_to_string($type->{'type'});
        return sprintf("(bf %s %s)",
                       $bf_type,
                       $type->{'width'});
    }
    if ($tag eq 'union') {
        return $type->{'name'};
    }
    if ($tag eq 'function-pointer') {
        return "(p void)";
    }
    my $mapped_type = $TYPE_MAP{$tag};
    if ($mapped_type) {
        if ($mapped_type =~ /^\(/) {
            $IMPORTS{'stdlib'} = 1;
        }
        return $mapped_type;
    }
    if ($tag eq 'struct') {
        my $casing_fn = $CASING_MAP{$CASING};
        my $new_name = $casing_fn->($type->{'name'});
        return $new_name;
    }

    my $casing_fn = $CASING_MAP{$CASING};
    my $new_name = $casing_fn->($tag);
    return $new_name;
}

sub type_to_flat_string
{
    my ($type) = @_;

    my $str = type_to_string($type);
    $str =~ tr/() /   /;
    $str =~ s/ //g;

    return $str;
}

my %SC_MAP = (
    'static' => 'intern',
    'none'   => 'extern-c',
    'extern' => 'extern-c',
);

sub storage_class_to_string
{
    return $SC_MAP{$_[0]};
}

sub process_function
{
    my ($data) = @_;

    our $in_function = 1;

    my @params =
        map { sprintf("(%s %s)", $_->{'name'},
                                 type_to_string($_->{'type'})) }
            @{$data->{'parameters'}};
    if (not @params) {
        @params = 'void';
    }
    my $param_str = "(".(join ' ', @params).")";

    sprintf("(def %s (fn %s %s %s))",
            $data->{'name'},
            storage_class_to_string($data->{'storage_class'}
                                 || $data->{'storage-class'}),
            type_to_string($data->{'return-type'}),
            $param_str);
}

sub process_variable
{
    my ($data) = @_;

    sprintf("(def %s (var extern %s))",
            $data->{'name'},
            type_to_string($data->{'type'}));
}

sub process_const
{
    my ($data) = @_;

    sprintf("(def %s (var intern %s%s))",
            $data->{'name'},
            type_to_string($data->{'type'}),
            ($data->{'value'} ? ' '.$data->{'value'} : ''));
}

sub process_struct
{
    my ($data) = @_;

    my @fields =
        map { sprintf("(%s %s)", $_->{'name'},
                                 type_to_string($_->{'type'})) }
            @{$data->{'fields'}};
    my $field_str = (@fields ? " (".(join ' ', @fields).")" : "");

    sprintf("(def %s (struct extern%s))",
            $data->{'name'},
            $field_str);
}

sub process_enum
{
    my ($data) = @_;

    $IMPORTS{'enum'} = 1;

    my @fields =
        map { sprintf("(%s %s)", $_->{'name'}, $_->{'value'}) }
            @{$data->{'fields'}};
    my $field_str = (@fields ? " (".(join ' ', @fields).")" : "");

    sprintf("(def-enum %s extern int%s)",
            $data->{'name'},
            $field_str);
}

sub process_typedef
{
    my ($data) = @_;

    if ($data->{'type'}->{'tag'} eq 'struct') {
        $data->{'type'}->{'name'} = $data->{'name'};
        return process_struct($data->{'type'});
    }
    
    my $type = type_to_string($data->{'type'});
    if (not ($type eq 'void')) {
        sprintf("(def %s (struct extern ((a %s))))",
                $data->{'name'},
                $type);
    }
}

sub process_union
{
    my ($data) = @_;

    $IMPORTS{'variant'} = 1;

    my $name = $data->{'name'};

    my @constructors =
        map { sprintf("(%s-%s ((value %s)))",
                      $name,
                      type_to_flat_string($_->{'type'}),
                      type_to_string($_->{'type'})) }
            @{$data->{'fields'}};
    my $constructor_str = join ' ', @constructors;

    sprintf("(def-variant %s (%s))",
            $name,
            $constructor_str);
}

sub name_to_parts
{
    my ($name) = @_;

    if ($name =~ /_/) {
        my @parts = map { lc $_ } split /_/, $name;
        return @parts;
    }
    if ($name =~ /[a-z0-9][A-Z0-9]/) {
        my @parts;
        for (;;) {
            my $used = 0;
            while ($name =~ s/([A-Z0-9]+[a-z0-9]+)$//) {
                unshift @parts, $1;
                $used = 1;
            }
            while ($name =~ s/([a-z])([A-Z0-9]+)$/$1/) {
                unshift @parts, $2;
                $used = 1;
            }
            if (not $used) {
                last;
            }
        }
        if ($name) {
            unshift @parts, $name;
        }
        return @parts;
    }
    return ($name);
}

sub casing_none
{
    my ($name) = @_;

    return $name;
}

sub casing_standard
{
    my ($name) = @_;

    return join '_', map { lc $_ } name_to_parts($name);
}

sub casing_camel
{
    my ($name) = @_;

    return join '', map { ucfirst lc $_ } name_to_parts($name);
}

sub casing_lisp
{
    my ($name) = @_;

    return join '-', map { lc $_ } name_to_parts($name);
}

sub print_binding
{
    my ($binding) = @_;

    my ($name) = ($binding =~ /^\(.*? (.*?) /);
    my $casing_fn = $CASING_MAP{$CASING};
    my $new_name = $casing_fn->($name);
    $binding =~ s/ (.*?) / $new_name /;

    print $binding,"\n";

    return 1;
}

my %PROCESS_MAP = (
    function => \&process_function,
    extern   => \&process_variable,
    struct   => \&process_struct,
    const    => \&process_const,
    enum     => \&process_enum,
    typedef  => \&process_typedef,
    union    => \&process_union,
);

sub main
{
    my ($namespaces) = @_;

    if (not $CASING_MAP{$CASING}) {
        print STDERR "Casing is invalid: must be one of ".
                     (join ', ', keys %CASING_MAP)."\n";
        exit(10);
    }

    our $in_function = 0;

    my @bindings;

    while (defined (my $entry = <STDIN>)) {
        chomp $entry;
        if (($entry eq '[') or ($entry eq ']')) {
            next;
        }
        $entry =~ s/,\s*$//;
        if (not $entry) {
            next;
        }
        my $data = decode_json($entry);
        if (@ARGV) {
            my $path = $data->{'location'};
            my $name = fileparse($path, qr/\.[^.]*/);  
            my $arg = $ARGV[0];
            if ($name ne $arg) {
                next;
            }
        }
        my $tag = $data->{'tag'};
        if ($PROCESS_MAP{$tag}) {
            my $result = $PROCESS_MAP{$tag}->($data);
            if ($result) {
                push @bindings, $result;
            }
        } else {
            warn "unable to process tag '$tag'";
        }
    }

    my @imports = sort keys %IMPORTS;
    if (@imports) {
        for my $import (@imports) {
            print "(import $import)\n";
        }
        print "\n";
    }

    my %by_namespace =
        map { $_ => [] }
            @{$namespaces};
    my @no_namespace;
    BINDING: for my $binding (@bindings) {
        my ($name) = ($binding =~ /^\(.*? (.*?) /);
        for my $namespace (@{$namespaces}) {
            if ($name =~ /^${namespace}/) {
                $name =~ s/^${namespace}//;
                $binding =~ s/ (.*?) / $name /;
                push @{$by_namespace{$namespace}}, $binding;
                next BINDING;
            }
        }
        push @no_namespace, $binding;
    }

    for my $namespace (@{$namespaces}) {
        my @ns_bindings = @{$by_namespace{$namespace}};
        if (@ns_bindings) {
            print "(namespace $namespace \n";
            for my $binding (@bindings) {
                print_binding($binding);
            }
            print ")\n";
        }
    }

    for my $binding (@no_namespace) {
        print_binding($binding);
    }
}

my @namespaces;
GetOptions("namespace=s", \@namespaces,
           "casing=s", \$CASING);
if (not $CASING) {
    $CASING = 'none';
}

main(\@namespaces);

1;
