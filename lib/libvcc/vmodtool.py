#!/usr/bin/env python
#-
# Copyright (c) 2010-2016 Varnish Software
# All rights reserved.
#
# Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.

"""
Read the vmod.vcc file (inputvcc) and produce:
	vmod_if.h -- Prototypes for the implementation
	vmod_if.c -- Magic glue & datastructures to make things a VMOD.
	vmod_${name}.rst -- Extracted documentation
"""

# This script should work with both Python 2 and Python 3.
from __future__ import print_function

import os
import sys
import re
import optparse
import unittest
import random
from os import fdopen, rename, unlink
from os.path import dirname, exists, join, realpath
from pprint import pprint, pformat
from tempfile import mkstemp

rstfmt=False

ctypes = {
	'ACL':		"VCL_ACL",
	'BACKEND':	"VCL_BACKEND",
	'BLOB':		"VCL_BLOB",
	'BODY':		"VCL_BODY",
	'BOOL':		"VCL_BOOL",
	'BYTES':	"VCL_BYTES",
	'DURATION':	"VCL_DURATION",
	'ENUM':		"VCL_ENUM",
	'HEADER':	"VCL_HEADER",
	'HTTP':		"VCL_HTTP",
	'INT':		"VCL_INT",
	'IP':		"VCL_IP",
	'PRIV_CALL':	"struct vmod_priv *",
	'PRIV_VCL':	"struct vmod_priv *",
	'PRIV_TASK':	"struct vmod_priv *",
	'PRIV_TOP':	"struct vmod_priv *",
	'PROBE':	"VCL_PROBE",
	'REAL':		"VCL_REAL",
	'STEVEDORE':	"VCL_STEVEDORE",
	'STRING':	"VCL_STRING",
	'STRING_LIST':	"const char *, ...",
	'TIME':		"VCL_TIME",
	'VOID':		"VCL_VOID",
}

#######################################################################

def write_file_warning(fo, a, b, c):
	fo.write(a + "\n")
	fo.write(b + " NB:  This file is machine generated, DO NOT EDIT!\n")
	fo.write(b + "\n")
	fo.write(b + " Edit vmod.vcc and run make instead\n")
	fo.write(c + "\n\n")

def write_c_file_warning(fo):
	write_file_warning(fo, "/*", " *", " */")

def write_rst_file_warning(fo):
	write_file_warning(fo, "..", "..", "..")

def write_rst_hdr(fo, s, below="-", above=None):
	if above != None:
		fo.write(above * len(s) + "\n")
	fo.write(s + "\n")
	if below != None:
		fo.write(below * len(s) + "\n")

#######################################################################

def lwrap(s, width=64):
	"""
	Wrap a C-prototype like string into a number of lines.
	"""
	l = []
	p = ""
	while len(s) > width:
		y = s[:width].rfind(',')
		if y == -1:
			y = s[:width].rfind('(')
		if y == -1:
			break
		l.append(p + s[:y + 1])
		s = s[y + 1:].lstrip()
		p = "    "
	if len(s) > 0:
		l.append(p + s)
	return l

def quote(s):
	return s.replace("\"", "\\\"")

def indent(p,n):
	n = len(p.expandtabs()) + n
	p = "\t" * int(n / 8)
	p += " " * int(n % 8)
	return p

#######################################################################

def is_c_name(s):
	return re.match("^[a-zA-Z][a-zA-Z0-9_]*$", s) is not None


class ParseError(Exception):
	"An error reading the input file."
	pass

class FormatError(Exception):
	"""
	Raised if the content of the (otherwise well-formed) input file
	is invalid.
	"""
	def __init__(self, msg, details):
		self.msg = msg
		self.details = details
		Exception.__init__(self)

#######################################################################

def err(str, warn=True):
	if opts.strict or not warn:
		print("ERROR: " + str, file = sys.stderr)
		exit(1)
		raise FormatError(str, "")
	else:
		print("WARNING: " + str, file = sys.stderr)

def fmt_cstruct(fo, mn, x):
	a = "\ttd_" + mn + "_" + x
	while len(a.expandtabs()) < 40:
		a += "\t"
	fo.write("%s*%s;\n" % (a, x))

#######################################################################

class ctype(object):
	def __init__(self, vt, ct):
		self.vt = vt
		self.ct = ct
		self.nm = None
		self.defval = None
		self.spec = None

	def __str__(self):
		s = "<" + self.vt
		if self.nm != None:
			s += " " + self.nm
		if self.defval != None:
			s += " VAL=" + self.defval
		if self.spec != None:
			s += " SPEC=" + str(self.spec)
		return s + ">"

	def vcl(self):
		if self.vt == "STRING_LIST":
			return "STRING"
		if self.spec == None:
			return self.vt
		return self.vt + " {" + ",".join(self.spec) + "}"

	def specstr(self, fo, p):
		fo.write(p + '"' + self.vt)
		fo.write('\\0"\n')
		p = indent(p, 4)
		if self.spec != None:
			fo.write(p + '"\\1"\n')
			p = indent(p, 4)
			for i in self.spec:
				fo.write(p + '"' + i + '\\0"\n')
			p = indent(p, -4)
			# This terminating \1 is necessary to ensure that
			# a prototype always ends with three \0's
			fo.write(p + '"\\1\\0"\n')
		if self.nm != None:
			fo.write(p + '"\\2" "' + self.nm + '\\0"\n')
		if self.defval != None:
			fo.write(p + '"\\3" "' + quote(self.defval) + '\\0"\n')

def vtype(txt):
	j = len(txt)
	for i in (',', ' ', '\n', '\t'):
		x = txt.find(i)
		if x > 0:
			j = min(j, x)
	t = txt[:j]
	r = txt[j:].lstrip()
	if t not in ctypes:
		err("Did not recognize type <%s>" % txt)
	ct = ctype(t, ctypes[t])
	if t != "ENUM":
		return ct, r
	assert r[0] == '{'
	e = r[1:].split('}', 1)
	r = e[1].lstrip()
	e = e[0].split(',')
	ct.spec = []
	for i in e:
		ct.spec.append(i.strip())
	return ct, r

def arg(txt):
	a,s = vtype(txt)
	if len(s) == 0 or s[0] == ',':
		return a,s

	i = s.find('=')
	j = s.find(',')
	if j < 0:
		j = len(s)
	if j < i:
		i = -1
	if i < 0:
		i = s.find(',')
		if i < 0:
			i = len(s)
		a.nm = s[:i].rstrip()
		s = s[i:]
		return a, s

	a.nm = s[:i].rstrip()
	s = s[i+1:].lstrip()
	if s[0] == '"' or s[0] == "'":
		m = re.match("(['\"]).*?(\\1)", s)
		if not m:
			err("Unbalanced quote")
		a.defval = s[:m.end()]
		s = s[m.end():]
	else:
		i = s.find(',')
		if i < 0:
			i = len(s)
		a.defval = s[:i].rstrip()
		s = s[i:]

	return a,s

# XXX cant have ( or ) in an argument default value
class prototype(object):
	def __init__(self, st, retval=True, prefix=""):
		l = st.line[1]
		while True:
			a1 = l.count("(")
			a2 = l.count(")")
			if a1 > 0 and a1 == a2:
				break
			n = st.doc.split("\n", 1)
			l += n[0]
			st.doc = n[1]

		if retval:
			self.retval,s = vtype(l)
		else:
			self.retval = None
			s = l
		i = s.find("(")
		assert i > 0
		self.name = prefix + s[:i].strip()
		s = s[i:].strip()
		assert s[0] == "("
		assert s[-1] == ")"
		s = s[1:-1].lstrip()
		self.args = []
		while len(s) > 0:
			a,s = arg(s)
			self.args.append(a)
			s = s.lstrip()
			if len(s) == 0:
				break;
			assert s[0] == ','
			s = s[1:].lstrip()

	def cname(self):
		return self.name.replace(".", "_")

	def vcl_proto(self, short):
		s = ""
		if self.retval != None:
			s += self.retval.vcl() + " "
		s += self.name + "("
		l = []
		for i in self.args:
			t = i.vcl()
			if not short:
				if i.nm != None:
					t += " " + i.nm
				if i.defval != None:
					t += "=" + i.defval
			l.append(t)
		s += ", ".join(l) + ")"
		return s

	def c_ret(self):
		return self.retval.ct

	def c_args(self):
		if len(self.args) == 0:
			return ""
		l = [""]
		for i in self.args:
			l.append(i.ct)
		return ", ".join(l)

	def specstr(self, fo, cfunc, p):
		if self.retval == None:
			fo.write(p + '"VOID\\0"\n')
		else:
			self.retval.specstr(fo, p)
		fo.write(p + '"' + cfunc + '\\0"\n')
		p = indent(p, 4)
		if self.args != None:
			for i in self.args:
				i.specstr(fo, p)
		fo.write(p + '"\\0"\n')

#######################################################################

class stanza(object):
	def __init__(self, l0, doc, vcc):
		self.line = l0
		if len(doc) == 1:
			self.doc = doc[0]
		else:
			self.doc = ""
		self.vcc = vcc
		self.rstlbl = None
		self.methods = None
		self.proto = None
		self.parse()

	def dump(self):
		print(type(self), self.line)

	def rstfile(self, fo, man):
		if self.rstlbl != None:
			fo.write(".. _" + self.rstlbl + ":\n\n")

		self.rsthead(fo, man)
		self.rstmid(fo, man)
		self.rsttail(fo, man)

	def rsthead(self, fo, man):
		if self.proto == None:
			return
		if rstfmt:
			s = self.proto.vcl_proto(short=False)
			write_rst_hdr(fo, s, '-')
		else:
			write_rst_hdr(fo, self.proto.name, '-')
			s = self.proto.vcl_proto(short=False)
			fo.write("\n::\n\n\t%s\n" % s)

	def rstmid(self, fo, man):
		fo.write(self.doc + "\n")

	def rsttail(self, fo, man):
		return

	def hfile(self, fo):
		return

	def cstruct(self, fo):
		return

	def cstruct_init(self, fo):
		return

	def specstr(self, fo):
		return

#######################################################################

class s_module(stanza):
	def parse(self):
		a = self.line[1].split(None, 2)
		self.vcc.modname = a[0]
		self.vcc.mansection = a[1]
		self.vcc.moddesc = a[2]
		self.rstlbl = "vmod_%s(%s)" % (
		    self.vcc.modname,
		    self.vcc.mansection
		)
		self.vcc.contents.append(self)

	def rsthead(self, fo, man):

		write_rst_hdr(fo, "vmod_" + self.vcc.modname, "=", "=")
		fo.write("\n")

		write_rst_hdr(fo, self.vcc.moddesc, "-", "-")

		fo.write("\n")
		fo.write(":Manual section: " + self.vcc.mansection + "\n")

		fo.write("\n")
		write_rst_hdr(fo, "SYNOPSIS", "=")
		fo.write("\n")
		fo.write('import %s [from "path"] ;\n' % self.vcc.modname)
		fo.write("\n")

	def rsttail(self, fo, man):

		write_rst_hdr(fo, "CONTENTS", "=")
		fo.write("\n")

		if man:
			for i in self.vcc.contents[1:]:
				if i.rstlbl == None:
					continue
				fo.write("* %s\n" %
				    i.proto.vcl_proto(short=True))
			fo.write("\n")
			return

		l = []
		for i in self.vcc.contents[1:]:
			j = i.rstlbl
			if j != None:
				l.append([j.split("_", 1)[1], j])
			if i.methods == None:
				continue
			for x in i.methods:
				j = x.rstlbl
				l.append([j.split("_", 1)[1], j])

		l.sort()
		for i in l:
			fo.write("* :ref:`%s`\n" % i[1])
		fo.write("\n")

class s_event(stanza):
	def parse(self):
		self.event_func = self.line[1]
		self.vcc.contents.append(self)

	def rstfile(self, fo, man):
		if len(self.doc) != 0:
			err("Not emitting .RST for $Event %s\n" %
			    self.event_func)

	def hfile(self, fo):
		fo.write("#ifdef VCL_MET_MAX\n")
		fo.write("vmod_event_f %s;\n" % self.event_func)
		fo.write("#endif\n")

	def cstruct(self, fo):
		fo.write("\tvmod_event_f\t\t\t*_event;\n")

	def cstruct_init(self, fo):
		fo.write("\t%s,\n" % self.event_func)

	def specstr(self, fo):
		fo.write('\t"$EVENT\\0"\n\t    "Vmod_%s_Func._event",\n\n' %
		    self.vcc.modname)

class s_function(stanza):
	def parse(self):
		self.proto = prototype(self)
		self.rstlbl = "func_" + self.proto.name
		self.vcc.contents.append(self)

	def hfile(self, fo):
		fn = "vmod_" + self.proto.name
		s = "%s %s(VRT_CTX" % (self.proto.c_ret(), fn)
		s += self.proto.c_args() + ");"
		for i in lwrap(s):
			fo.write(i + "\n")

	def cfile(self, fo):
		fn = "td_" + self.vcc.modname + "_" + self.proto.name
		s = "typedef %s %s(VRT_CTX" % (self.proto.c_ret(), fn)
		s += self.proto.c_args() + ");"
		for i in lwrap(s):
			fo.write(i + "\n")

	def cstruct(self, fo):
		fmt_cstruct(fo, self.vcc.modname, self.proto.cname())

	def cstruct_init(self, fo):
		fo.write("\tvmod_" + self.proto.cname() + ",\n")

	def specstr(self, fo):
		fo.write('\t"$FUNC\\0"\t"%s.%s\\0"\n\n' %
		    (self.vcc.modname, self.proto.name))
		self.proto.specstr(fo, 'Vmod_%s_Func.%s' %
		    (self.vcc.modname, self.proto.cname()), "\t    ")
		fo.write('\t    "\\0",\n\n')

class s_object(stanza):
	def parse(self):
		self.proto = prototype(self, retval=False)
		self.rstlbl = "obj_" + self.proto.name
		self.vcc.contents.append(self)
		self.methods = []

	def rsthead(self, fo, man):
		if rstfmt:
			s = self.proto.vcl_proto(short=False)
			write_rst_hdr(fo, "new OBJ = " + s, '=')
		else:
			write_rst_hdr(fo, self.proto.name, '-')
			s = "new OBJ = " + self.proto.vcl_proto(short=False)
			fo.write("\n::\n\n\t%s\n" % s)

		fo.write(self.doc + "\n")

		for i in self.methods:
			i.rstfile(fo, man)

	def rstmid(self, fo, man):
		return

	def chfile(self, fo, h):
		sn = "vmod_" + self.vcc.modname + "_" + self.proto.name
		fo.write("struct %s;\n" % sn)

		if h:
			def p(x):
				return x + " vmod_"
		else:
			def p(x):
				return "typedef " + x + \
				    " td_%s_" % self.vcc.modname

		s = p("VCL_VOID") + "%s__init(VRT_CTX, " % self.proto.name
		s += "struct %s **, const char *" % sn
		s += self.proto.c_args() + ");"
		for i in lwrap(s):
			fo.write(i + "\n")

		s = p("VCL_VOID")
		s += "%s__fini(struct %s **);" % (self.proto.name, sn)
		for i in lwrap(s):
			fo.write(i + "\n")

		for i in self.methods:
			cn = i.proto.cname()
			s = p(i.proto.c_ret())
			s += "%s(VRT_CTX, struct %s *" % (cn, sn)
			s += i.proto.c_args() + ");"
			for i in lwrap(s):
				fo.write(i + "\n")
		fo.write("\n")

	def hfile(self, fo):
		self.chfile(fo, True)

	def cfile(self, fo):
		self.chfile(fo, False)

	def cstruct(self, fo):
		td = "td_" + self.vcc.modname + "_" + self.proto.name + "_"
		fmt_cstruct(fo, self.vcc.modname, self.proto.name + "__init")
		fmt_cstruct(fo, self.vcc.modname, self.proto.name + "__fini")
		for i in self.methods:
			i.cstruct(fo)

	def cstruct_init(self, fo):
		p = "\tvmod_"
		fo.write(p + self.proto.name + "__init,\n")
		fo.write(p + self.proto.name + "__fini,\n")
		for i in self.methods:
			i.cstruct_init(fo)
		fo.write("\n")

	def specstr(self, fo):

		fo.write('\t"$OBJ\\0"\t"%s.%s\\0"\n\n' %
		    (self.vcc.modname, self.proto.name))

		fo.write('\t    "struct vmod_%s_%s\\0"\n' %
		    (self.vcc.modname, self.proto.name))
		fo.write("\n")

		self.proto.specstr(fo, 'Vmod_%s_Func.%s__init' %
		    (self.vcc.modname, self.proto.name), '\t    ')
		fo.write('\t    "\\0"\n\n')

		fo.write('\t    "VOID\\0"\n')
		fo.write('\t    "Vmod_%s_Func.%s__fini\\0"\n' %
		    (self.vcc.modname, self.proto.name))
		fo.write('\t\t"\\0"\n')
		fo.write('\t    "\\0"\n\n')

		for i in self.methods:
			i.specstr(fo)

		fo.write('\t    "\\0",\n\n')

	def dump(self):
		super(s_object, self).dump()
		for i in self.methods:
			i.dump()

class s_method(stanza):
	def parse(self):
		p = self.vcc.contents[-1]
		assert type(p) == s_object
		self.proto = prototype(self, prefix=p.proto.name)
		self.rstlbl = "func_" + self.proto.name
		p.methods.append(self)

	def cstruct(self, fo):
		fmt_cstruct(fo, self.vcc.modname, self.proto.cname())

	def cstruct_init(self, fo):
		fo.write('\t' + "vmod_" + self.proto.cname() + ",\n")

	def specstr(self, fo):
		fo.write('\t    "%s.%s\\0"\n' %
		    (self.vcc.modname, self.proto.name))
		self.proto.specstr(fo, 'Vmod_%s_Func.%s' %
		    (self.vcc.modname, self.proto.cname()), '\t\t')
		fo.write('\t\t"\\0"\n\n')

#######################################################################

class vcc(object):
	def __init__(self, inputvcc, rstdir, outputprefix):
		self.inputfile = inputvcc
		self.rstdir = rstdir
		self.pfx = outputprefix
		self.contents = []
		self.commit_files = []
		self.copyright = None

	def commit(self):
		for i in self.commit_files:
			os.rename(i + ".tmp", i)

	def parse(self):
		a = open(self.inputfile, "r").read()
		a = a.split("\n$")
		for i in range(len(a)):
			b = a[i].split("\n", 1)
			c = b[0].split(None, 1)

			if i == 0:
				if c[0] == "$Module":
					s_module(c, b[1:], self)
				else:
					self.copyright = a[0]
				continue
			if i == 1 and self.copyright != None:
				if c[0] != "Module":
					err("$Module must be first stanze")
			if c[0] == "Module":
				s_module(c, b[1:], self)
			elif c[0] == "Event":
				s_event(c, b[1:], self)
			elif c[0] == "Function":
				s_function(c, b[1:], self)
			elif c[0] == "Object":
				s_object(c, b[1:], self)
			elif c[0] == "Method":
				s_method(c, b[1:], self)
			else:
				err("Unknown stanze $%s" % c[0])

	def rst_copyright(self, fo):
		write_rst_hdr(fo, "COPYRIGHT", "=")
		fo.write("\n::\n\n")
		a = self.copyright
		a = a.replace("\n#", "\n ")
		if a[:2] == "#\n":
			a = a[2:]
		if a[:3] == "#-\n":
			a = a[3:]
		fo.write(a + "\n")

	def rstfile(self, man=False):
		fn = join(self.rstdir, "vmod_" + self.modname)
		if man:
			fn += ".man"
		fn += ".rst"
		self.commit_files.append(fn)
		fo = open(fn + ".tmp", "w")
		write_rst_file_warning(fo)
		fo.write(".. role:: ref(emphasis)\n\n")

		for i in self.contents:
			i.rstfile(fo, man)

		if self.copyright != None:
			self.rst_copyright(fo)

		fo.close()

	def hfile(self):
		fn = self.pfx + ".h"
		self.commit_files.append(fn)
		fo = open(fn + ".tmp", "w")
		write_c_file_warning(fo)
		fo.write("struct vmod_priv;\n")
		fo.write("\n")
		fo.write("extern const struct vmod_data Vmod_%s_Data;\n" %
			(self.modname))
		fo.write("\n")

		for j in self.contents:
			j.hfile(fo)
		fo.close()

	def cstruct(self, fo, csn):

		fo.write("\n%s {\n" % csn)
		for j in self.contents:
			j.cstruct(fo)
		fo.write("};\n")

	def cstruct_init(self, fo, csn):
		fo.write("\nstatic const %s Vmod_Func = {\n" % csn)
		for j in self.contents:
			j.cstruct_init(fo)
		fo.write("};\n")

	def specstr(self, fo):
		fo.write("\n/*lint -save -e786 -e840 */\n")
		fo.write("static const char * const Vmod_Spec[] = {\n")

		for j in self.contents:
			j.specstr(fo)
		fo.write("\t0\n")
		fo.write("};\n")
		fo.write("/*lint -restore */\n")

	def api(self, fo):
		fo.write("\n/*lint -esym(759, Vmod_%s_Data) */\n" % (self.modname))
		fo.write("const struct vmod_data Vmod_%s_Data = {\n" %
		    self.modname)
		fo.write("\t.vrt_major =\tVRT_MAJOR_VERSION,\n")
		fo.write("\t.vrt_minor =\tVRT_MINOR_VERSION,\n")
		fo.write('\t.name =\t\t"%s",\n' % self.modname)
		fo.write('\t.func =\t\t&Vmod_Func,\n')
		fo.write('\t.func_len =\tsizeof(Vmod_Func),\n')
		fo.write('\t.proto =\tVmod_Proto,\n')
		fo.write('\t.spec =\t\tVmod_Spec,\n')
		fo.write('\t.abi =\t\tVMOD_ABI_Version,\n')
		# NB: Sort of hackish:
		# Fill file_id with random stuff, so we can tell if
		# VCC and VRT_Vmod_Init() dlopens the same file
		#
		fo.write("\t.file_id =\t\"")
		for i in range(32):
			fo.write("%c" % random.randint(0x40, 0x5a))
		fo.write("\",\n")
		fo.write("};\n")

	def cfile(self):
		fn = self.pfx + ".c"
		self.commit_files.append(fn)
		fo = open(fn + ".tmp", "w")
		write_c_file_warning(fo)

		fn2 = fn + ".tmp2"

		fo.write('#include "config.h"\n')
		fo.write('#include <stdio.h>\n')
		for i in ["vdef", "vcl", "vrt", self.pfx, "vmod_abi"]:
			fo.write('#include "%s.h"\n' % i)

		fo.write("\n")

		fx = open(fn2, "w")

		for i in self.contents:
			if type(i) == s_object:
				i.cfile(fo)
				i.cfile(fx)

		fx.write("/* Functions */\n")
		for i in self.contents:
			if type(i) == s_function:
				i.cfile(fo)
				i.cfile(fx)

		csn = "Vmod_%s_Func" % self.modname

		self.cstruct(fo, "struct " + csn)

		self.cstruct(fx, "struct " + csn)

		fo.write("\n/*lint -esym(754, Vmod_debug_Func::*) */\n")
		self.cstruct_init(fo, "struct " + csn)

		fx.close()

		fo.write("\nstatic const char Vmod_Proto[] =\n")
		fi = open(fn2)
		for i in fi:
			fo.write('\t"%s\\n"\n' % i.rstrip())
		fi.close()
		fo.write('\t"static struct %s %s;";\n' % (csn, csn))

		os.remove(fn2)

		self.specstr(fo)

		self.api(fo)

		fo.close()

#######################################################################

def runmain(inputvcc, rstdir, outputprefix):

	v = vcc(inputvcc, rstdir, outputprefix)
	v.parse()

	v.rstfile(man=False)
	v.rstfile(man=True)
	v.hfile()
	v.cfile()

	v.commit()

if __name__ == "__main__":
	usagetext = "Usage: %prog [options] <vmod.vcc>"
	oparser = optparse.OptionParser(usage=usagetext)

	oparser.add_option('-N', '--strict', action='store_true', default=False,
	    help="Be strict when parsing the input file")
	oparser.add_option('-o', '--output', metavar="prefix", default='vcc_if',
	    help='Output file prefix (default: "vcc_if")')
	oparser.add_option('-w', '--rstdir', metavar="directory", default='.',
	    help='Where to save the generated RST files (default: ".")')
	oparser.add_option('', '--runtests', action='store_true', default=False,
	    dest="runtests", help=optparse.SUPPRESS_HELP)
	(opts, args) = oparser.parse_args()

	if opts.runtests:
		# Pop off --runtests, pass remaining to unittest.
		del sys.argv[1]
		unittest.main()
		exit()

	i_vcc = None
	if len(args) == 1 and exists(args[0]):
		i_vcc = args[0]
	elif exists("vmod.vcc"):
		if not i_vcc:
			i_vcc = "vmod.vcc"
	else:
		print("ERROR: No vmod.vcc file supplied or found.",
		    file=sys.stderr)
		oparser.print_help()
		exit(-1)

	runmain(i_vcc, opts.rstdir, opts.output)
