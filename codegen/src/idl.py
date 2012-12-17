#! /usr/bin/env python

## Copyright (c) 2012 Aldebaran Robotics. All rights reserved.

""" Code parsing and generator tool.

    Representations used:
    - IDL: XML file describing the interface
    - RAW: Internal representation of the IDL
    raw: (methods, signals)
    methods: [method]
    signals: [signal]
    method: (name, [argtype], rettype)
    signal: (name, [argtype])

    Code parser:
    - Invoke Doxygen and parse its XML output to produce an IDL file.

    Code generators from RAW to C++:
    - interface: An interface that both proxy and service can implement.
    - proxy: Specialized proxy, with or without using the interface
    - service skeleton
    - service bouncer: Implementation of the interface that bounces to an
      existing class
    - service and type registration: Code that fills an ObjectTypeBuilder and
      registers it.
"""

from xml.etree import ElementTree as etree
import sys
import argparse
import tempfile
import os
import subprocess
import shutil
import re

TYPE_MAP = {
  'unsigned int': 'uint',
  'unsigned long': 'uint64',
  'unsigned short': 'ushort',
  'unsigned char': 'uchar',
  'long': 'int64',
  'char*': 'string',
  'qi::GenericValue': 'dynamic',
}

REV_MAP = {
    'uint' : 'unsigned int',
    'uint64' : 'unsigned long',
    'dynamic': 'qi::GenericValue',
    'string' : 'std::string',
    'int64'  : 'qi::int64_t',
}

def idltype_to_cxxtype(t):
  """ Return the C++ type to use for idl type t
  """
  t = t.replace('{', 'std::map<').replace('}', ' >')
  t = t.replace('[', 'std::vector<').replace(']', ' >')
  for e in REV_MAP:
    t = t.replace(e, REV_MAP[e])
  return t

def parse_toplevel_comma(txt):
  """ Split given string on top-level commas (not within <>)
  """
  components = []
  level = 0
  p = 0
  while p < len(txt):
    if txt[p] == '>':
      level = level - 1
    if txt[p] == '<':
      level = level + 1
    if txt[p] == ',' and level == 0:
      components.append(txt[0: p])
      txt = txt[p+1:]
      p = 0
    else:
      p = p+1
  components.append(txt)
  return components

def cxx_type_parse(txt):
  """ Split a C++ type into basic components.
  Extracts template parameters.
  A type is extracted as a string, or (template-name, template-args, trailing-stuff)
  args is a list containing strings or (template-name, template-args, trailing)
  """
  components = parse_toplevel_comma(txt)
  results = []
  for t in components:
    substart = t.find('<')
    if substart != -1:
      #find matching
      count=1
      p = substart + 1
      while p < len(t) and count:
        if t[p] in '>}]':
          count = count - 1
        if t[p] in '<{[':
          count = count + 1
        p = p+1
      if count:
        print ("Parse error in " + t)
      elem = t[substart+1:p-1]
      subres = cxx_type_parse(elem)
      after = t[p+1:]
      results.append((t[0:substart], subres, after))
    else:
      results.append(t)
  return results

def cxx_parsed_to_sig(p):
  """ Convert a C++ type parsed by cxx_type_parse into the corresponding
      IDL signature.
  """
  if (type(p) == list):
    return ','.join(map(cxx_parsed_to_sig, p))
  if (type(p) == tuple):
    if re.search('vector$', p[0]):
      return p[0][0:-6] + "[" + cxx_parsed_to_sig(p[1]) + "]" + cxx_parsed_to_sig(p[2])
    elif re.search('map$', p[0]):
      return p[0][0:-3] + "{" + cxx_parsed_to_sig(p[1]) + "}" + cxx_parsed_to_sig(p[2])
    else: # unknown template
      return p[0] + "<" + p[1] + ">" + p[2]
  else:
    return p


def cxx_type_to_signature(t):
  """ Convert the string representation of a C++ type into the corresponding
  IDL signature
  """
  # Drop const and ref.
  # Drop namespace std (assume any class named vector is...a vector)
  t = t.replace('const ', '').replace("&", '').replace('std::', '')
  # Drop all spaces that do not separate identifiers
  t = re.sub(r"\s([^a-zA-Z])", r"\1", t)
  t = re.sub(r"([^a-zA-Z])\s", r"\1", t)
  t = t.strip()
  #Known type conversion
  for e in TYPE_MAP:
    t = re.sub(e, TYPE_MAP[e], t)
  #Container handling
  #For correct result in presence of containers of containers,
  #we need to parse the type almost fully
  #Huge hack, we do not realy parse 'a,b' in template
  parsed = cxx_type_parse(t)
  sig = cxx_parsed_to_sig(parsed)
  return sig

def run_doxygen(files):
  """ Invoke doxygen on given source files or directories
  :param files: A list of file or directory to scan
  :result: the temporary directory where doxygen output is
  """
  tmp_dir = tempfile.mkdtemp()
  # Create Doxyfile in there
  doxyfile_path = os.path.join(tmp_dir, "Doxyfile")
  doxy = open(doxyfile_path, "w")
  doxy.write("GENERATE_XML=YES\n"
    + "GENERATE_HTML=NO\n"
    + "GENERATE_LATEX=NO\n"
    + "QUIET=YES\n"
    + "WARN_IF_UNDOCUMENTED   = NO\n"
    + "INPUT= " + " ".join(files) + "\n"
    + "OUTPUT_DIRECTORY= " + tmp_dir + "\n")
  doxy.close()
  # Invoke doxygen
  subprocess.call(["doxygen", doxyfile_path])
  return tmp_dir

def doxyxml_to_raw(doxy_dir):
  """ Convert doxygen output to internal RAW representation
  """
  # Parse the index to get all class names (and their functions)
  index_tree = etree.parse(os.path.join(doxy_dir, "xml", "index.xml"))
  class_index = dict()
  result = dict()
  for cls in index_tree.findall(".//compound[@kind='class']"):
    class_index[cls.find("name").text] = (cls.get('refid'), [f.find("name").text for f in cls.findall("member[@kind='function']")])
  for cls in class_index:
    class_id = class_index[cls][0]
    ctree = etree.parse(os.path.join(doxy_dir, "xml", class_id + ".xml"))
    class_root = ctree.find(".//compounddef[@id='" + class_id + "']")
    methods = []
    # Parse methods
    for m in class_root.findall("sectiondef[@kind='public-func']/memberdef[@kind='function']"):
      method_name = m.find("name").text
      rettype_raw = m.find("type").text
      if not rettype_raw:
        continue # constructor
      rettype = cxx_type_to_signature(rettype_raw)
      arg_nodes = m.findall("param")
      argstype_raw = []
      if arg_nodes is not None:
        argstype_raw = [a.find('type').text for a in arg_nodes]
      argstype = map(cxx_type_to_signature, argstype_raw)
      methods.append((method_name, argstype, rettype))
    signals = []
    # Parse signals
    for s in class_root.findall("sectiondef[@kind='public-attrib']/memberdef[@kind='variable']"):
      name = s.find("name").text
      t = s.find("type").text
      # Normalize spacing to ease matching below
      t = re.sub(r"\s([^a-zA-Z])", r"\1", t)
      t = re.sub(r"([^a-zA-Z])\s", r"\1", t)
      t = t.strip()
      match = re.match(r"(qi::)?Signal<[^(]+\((.*)\)>", t)
      if match:
        t = match.expand(r"\2")
        sig = cxx_type_to_signature(t)
        sig = parse_toplevel_comma(sig)
        signals.append((name, sig))

    result[cls] = (methods, signals)
  return result

def raw_to_idl(dstruct):
  """ Convert RAW to IDL XML format
  """
  root = etree.Element('IDL')
  for cls in dstruct:
    e = etree.SubElement(root, 'class', name=cls)
    (methods, signals) = dstruct[cls]
    for method in methods:
      (method_name, args, ret) = method
      m = etree.SubElement(e, 'method', name=method_name)
      etree.SubElement(m, 'return', type=ret)
      for a in args:
        etree.SubElement(m, 'argument', type=a)
    for signal in signals:
      s = etree.SubElement(e, 'signal', name=signal[0])
      for a in signal[1]:
        etree.SubElement(s, 'argument', type=a)
  return root

def raw_to_text(dstruct):
  """ Convert RAW to human-readable text
  """
  result = ""
  for cls in dstruct:
    result += "class " + cls +"\n  methods\n"
    for method in dstruct[cls][0]:
      (method_name, args, ret) = method
      result += "    " + ret[0] + " " + method_name +"(" + ",".join(args) + ")\n"
    result += "  signals\n"
    for signal in dstruct[cls][1]:
      result += "    " + signal[0] + '(' + ','.join(signal[1]) + ')\n'
  return result

def method_to_cxx(method):
  """ Take a method from RAW representation, and return
      (declarationret, declarationargs, args), for example
      ("int", "int p1, std::string p2", "p1, p2")
  """
  iret = method[2]
  cret = idltype_to_cxxtype(iret)
  iargs = method[1]
  cargs = map(idltype_to_cxxtype, iargs)
  typedArgs = map(lambda x: cargs[x] + ' p' + str(x), range(len(cargs)))
  typedArgs = ','.join(typedArgs)
  argNames = map(lambda x: 'p' + str(x), range(len(cargs)))
  argNames = ','.join(argNames)
  return (cret, typedArgs, argNames)

def idl_to_raw(root):
  """ Convert IDL XML to internal RAW representation
  """
  result = dict()
  for cls in root.findall("class"):
    methods = []
    for m in cls.findall("method"):
      r = m.find("return").get("type")
      args = [a.get("type") for a in m.findall("argument")]
      methods.append((m.get('name'), args, r))
    signals = []
    for s in cls.findall("signal"):
      n = s.get('name')
      args = [a.get("type") for a in s.findall("argument")]
      signals.append((n, args))
    result[cls.get("name")] =  (methods, signals)
  return result

def raw_to_interface(class_name, data):
  """ Generate service interface class from RAW representation
  """
  skeleton = """
#ifndef @NAME@_INTERFACE_HPP
#define @NAME@_INTERFACE_HPP

#include <vector>
#include <string>
#include <map>

#include <qitype/signal.hpp>

class I@NAME@
{
  public:
    virtual ~I@NAME@() {}
@DECLS@
};

typedef boost::shared_ptr<I@NAME@> I@NAME@Ptr;

#endif
"""
  (methods, signals) = (data[0], data[1])
  methodsDecl = ''
  for method in methods:
    (cret, typedArgs, argNames) = method_to_cxx(method)
    method_name = method[0]
    methodsDecl += '    virtual %s %s (%s) = 0;\n' % (cret, method_name, typedArgs)
  signalsDecl = ''
  ctorDecl = []
  ctorInit = []
  for sig in signals:
    signature = ','.join(map(idltype_to_cxxtype, sig[1]))
    signalsDecl += '    qi::Signal<void(%s)> & %s;\n' % (signature, sig[0])
    ctorDecl.append('qi::Signal<void(%s)> & %s' % (signature, sig[0]))
    ctorInit.append('%s(%s)' % (sig[0], sig[0]))
  if len(ctorDecl):
    ctorDecl = ','.join(ctorDecl)
    ctorInit =  ':' + '\n      ,'.join(ctorInit)
  else:
    ctorDecl = ''
    ctorInit = ''
  ctor = '    I%s(%s)\n      %s\n    {}\n' % (class_name, ctorDecl, ctorInit)
  return skeleton.replace("@NAME@", class_name).replace("@DECLS@", ctor + methodsDecl + signalsDecl)

def raw_to_proxy(class_name, data, return_future, implement_interface, include):
  """ Generate C++ proxy code from RAW
  @param return_future have the declared functions return a Future
  @param implement_interface make the proxy inherit from an interface, that
         can be generated with raw_to_interface
  """
  skeleton = """
#ifndef @GARD@
#define @GARD@


#include <vector>
#include <string>
#include <map>

#include <qi/types.hpp>
#include <qitype/signal.hpp>
#include <qitype/genericobject.hpp>

@include@

static void signal_bridge(bool enable, qi::SignalBase::Link* link, qi::GenericObject* obj,
  qi::SignalBase* sig, const char* sigName);

#ifndef QI_GENERATED_PROXY_SIGNAL
#define QI_GENERATED_PROXY_SIGNAL
template<typename T> class ProxySignal: public qi::Signal<T>
{
public:
  ProxySignal(qi::SignalBase::OnSubscribers os, qi::GenericObject* obj, const std::string& name)
  : qi::Signal<T>(os)
  , _obj(obj)
  , _name(name)
  {
  }
  virtual void trigger(const qi::GenericFunctionParameters& params, qi::MetaCallType)
  {
    _obj->xMetaPost(_name + "::" + this->signature(), params);
  }
  qi::GenericObject* _obj;
  std::string _name;
};

#endif
class @className@Proxy @inherit@
{
public:
  @className@Proxy(qi::ObjectPtr obj)
  : _obj(obj)
@constructorInitList@
  {
@constructor@
  }
  qi::ObjectPtr asObject() { return _obj;}
   private:
    qi::ObjectPtr _obj;
   public:
@publicDecl@

@privateDecl@
};

typedef boost::shared_ptr<@className@Proxy> @className@ProxyPtr;

#ifndef QI_GENERATED_PROXY_CODE
#define QI_GENERATED_PROXY_CODE
static qi::GenericValuePtr signal_bounce(const std::vector<qi::GenericValuePtr>& args,
 qi::SignalBase* target)
{
  target->SignalBase::trigger(args);
  return qi::GenericValuePtr(qi::typeOf<void>(), 0);
}

static void signal_bridge(bool enable, qi::SignalBase::Link* link, qi::GenericObject* obj,
  qi::SignalBase* sig, const char* sigName)
{
  std::string signature = sigName + ("::" + sig->signature());
  if (enable)
    *link = obj->xConnect(signature, qi::SignalSubscriber(qi::makeDynamicGenericFunction(
      boost::bind(&signal_bounce, _1, sig))));
  else
    obj->disconnect(*link);
}

#endif

#endif //@GARD@
"""
  #generate methods
  (methods, signals) = (data[0], data[1])
  methodImpls = ""
  for method in methods:
    (cret, typedArgs, argNames) = method_to_cxx(method)
    method_name = method[0]
    if (return_future):
      cret = 'qi::FutureSync<' + cret + ' >'
    if argNames:
      argNames = ', ' + argNames # comma used in call
    #NOTE: should we return the future?
    methodImpls += '  ' + cret + " " + method_name + "(" + typedArgs + ") {\n    "
    if (cret != "void" or return_future):
      methodImpls += "return "
    methodImpls += '_obj->call<' + cret + ' >' + '("' + method_name + '"' + argNames + ");\n  }\n"
  signalDecl = ''
  signalDecl2 = ''
  ctor = ''
  ifaceCtor = []
  # Make  a Signal field for each signal, bridge it to backend in ctor
  for sig in signals:
    signalDecl += '  ProxySignal<void(' + ','.join(map(idltype_to_cxxtype, sig[1])) +')> ' + sig[0] + ';\n'
    signalDecl2 += '  qi::SignalBase::Link _link_' + sig[0] + ';\n'
    ctor += '  , {0}(boost::bind(&signal_bridge, _1, &_link_{0}, obj.get(), &{0}, "{0}"), obj.get(), "{0}")\n'.format(sig[0])
    ifaceCtor.append('%s' % (sig[0]))
  if implement_interface:
    ctor += '  , %s(%s)\n' % (class_name, ','.join(ifaceCtor))
  result = skeleton
  inherits = ''
  if implement_interface:
    inherits = ': public I' + class_name
  replace = {
      'GARD': '_' + class_name.upper() + '_PROXY_HPP_',
      'className': class_name,
      'publicDecl': methodImpls + signalDecl,
      'privateDecl': signalDecl2,
      'constructor': '',
      'constructorInitList': ctor,
      'inherit': inherits,
      'include': ''.join(['#include <' + x + '>\n' for x in include]),
  }
  for k in replace:
    result = result.replace('@' + k + '@', replace[k])
  return result

def raw_to_cxx_typebuild(class_name, data, registerToFactory):
  """ Generate a c++ file that registers the class to type system.
  """
  template = """
#include <qitype/objecttypebuilder.hpp>
#include <qitype/objectfactory.hpp>

namespace _qi_TYPE {
qi::ObjectTypeBuilder<TYPEService> builder;

qi::ObjectPtr makeOne(const std::string&)
{
  return builder.object(new TYPEService());
}

static int init()
{
ADVERTISE
  builder.registerType();
REGISTER
  return 0;
}
static int _init_ = init();
}
"""
  advertise = ""
  (methods, signals) = (data[0], data[1])
  for method in methods:
    method_name = method[0]
    advertise += '  builder.advertiseMethod("{0}", &{1}::{0});\n'.format(method_name, class_name)
  for s in signals:
    advertise += '  builder.advertiseEvent("{0}", &{1}::{0});\n'.format(s[0], class_name)
  register = ''
  if registerToFactory:
    register = '  qi::registerObjectFactory("{}", &makeOne);'.format(class_name)
  return template.replace('TYPE', class_name).replace('ADVERTISE', advertise).replace('REGISTER', register)

def raw_to_cxx_service_skeleton(class_name, data, implement_interface, include):
  """ Produce skeleton of C++ implementation of the service.
  """
  result = "#include <qitype/signal.hpp>\n#include <%s>\n\n" % include
  inherits = ''
  if implement_interface:
    inherits = ' : public I' + class_name
  result += "class %sService %s \n{\npublic:\n" % (class_name, inherits)
  (methods, signals) = (data[0], data[1])
  for method in methods:
    method_name = method[0]
    args = ','.join(map(idltype_to_cxxtype, method[1]))
    result += '  %s %s(%s);\n' % (
      idltype_to_cxxtype(method[2]),
      method_name,
      args
    )
  ifaceCtor = []
  for signal in signals:
    ifaceCtor.append('%s' % (signal[0]))
    result += '  qi::Signal<void(%s)> %s;\n' % (
      ','.join(map(idltype_to_cxxtype, signal[1])),
      signal[0]
    )
  if implement_interface:
    result += '  %sService() :%s(%s) {}\n' % (class_name, class_name, ','.join(ifaceCtor))
  result += '};\n\n'
  for method in methods:
    method_name = method[0]
    args = method[1]
    for i in range(len(args)):
      args[i] = idltype_to_cxxtype(args[i]) + ' p' + str(i)
    args = ','.join(args)
    result += '%s %s::%s(%s)\n{\n  // Implementation of %s\n}\n' % (
      idltype_to_cxxtype(method[2]),
      class_name,
      method_name,
      args,
      method_name
    )
  return result

def raw_to_cxx_service_bouncer(class_name, data, impl_name, include):
  """ Produce implementation of \p class_name interface bouncing to class
      \p impl_name.
  """
  skeleton = """
#include <vector>
#include <string>
#include <map>

#include <qi/types.hpp>
#include <qitype/signal.hpp>

@include@

class @name@Service: public I@name@
{
  public:
    @name@Service()
    : I@name@(@signalInit@)
    {}
@code@
  private:
  @impl@ _impl;
};
"""
  (methods, signals) = data
  signalInit = []
  for s in signals:
    signalInit.append('_impl.' + s[0])
  methodBounce = ''
  for method in methods:
    (ret, typedArgs, args) = method_to_cxx(method)
    method_name = method[0]
    methodBounce += '   %s %s(%s) { return _impl.%s(%s);}\n' % (ret, method_name, typedArgs, method_name, args)
  include = ''.join(['#include <' + x + '>\n' for x in include])
  return skeleton.replace(
    '@name@', class_name).replace(
    '@impl@', impl_name.replace('@', class_name)).replace(
    '@code@', methodBounce).replace(
    '@include@', include).replace(
    '@signalInit@', ','.join(signalInit))

def main(args):
  res = ''
  parser = argparse.ArgumentParser()
  parser.add_argument("--interface", "-i", help="Use interface mode", action='store_true')
  parser.add_argument("--output-file","-o", help="output file (stdout)")
  parser.add_argument("--output-mode","-m", default="idl", choices=["txt", "idl", "proxy", "proxyFuture", "cxxtype", "cxxtyperegister", "cxxskel", "cxxservice", "cxxservicebouncer", "interface"], help="output mode (stdout)")
  parser.add_argument("--include", "-I", default="", help="File to include in generated C++")
  parser.add_argument("--classes", "-c", default="*", help="Comma-separated list of classes to select")
  parser.add_argument("input", nargs='+', help="input file(s)")
  pargs = parser.parse_args(args)
  pargs.input = pargs.input[1:]
  # Step one: get raw from either IDL, or source files
  if len(pargs.input) == 1 and pargs.input[0][-3:] == 'idl':
    xml = etree.ElementTree(file=pargs.input[0]).getroot()
    raw = idl_to_raw(xml)
  else:
    doxy_dir = run_doxygen(pargs.input)
    raw = doxyxml_to_raw(doxy_dir)
    shutil.rmtree(doxy_dir)
  pargs.include = pargs.include.split(',')
  # Augment type mapping with what we will handle
  for c in raw:
    REV_MAP[c + 'Ptr'] = 'I' + c + 'Ptr'
  # Set output stream to file or stdout
  out = sys.stdout
  if pargs.output_file and pargs.output_file != "-" :
    out = open(pargs.output_file, "w")
  # Filter out classes present in raw
  if pargs.classes != '*':
    classes = pargs.classes.split(',')
    newraw = dict()
    for c in classes:
      if not c in raw:
        raise Exception("Requested class %s not found" % c)
      newraw[c] = raw[c]
    raw = newraw
  # Main switch on output mode
  if pargs.output_mode == "txt":
    res = raw_to_text(raw)
  elif pargs.output_mode == "idl":
    res = etree.tostring(raw_to_idl(raw))
  else: # Need to apply per-class function
    functions = []
    args = []
    if pargs.output_mode == "interface":
      functions = [raw_to_interface]
      args = [[]]
    elif pargs.output_mode == "proxy":
      functions = [raw_to_proxy]
      args = [[False, pargs.interface, pargs.include]]
    elif pargs.output_mode == "proxyFuture":
      functions = [raw_to_proxy]
      args = [[True, pargs.interface, pargs.include]]
    elif pargs.output_mode == "cxxtype":
      functions = [raw_to_cxx_typebuild]
      args = [[False]]
    elif pargs.output_mode == "cxxtyperegister":
      functions = [raw_to_cxx_typebuild]
      args = [[True]]
    elif pargs.output_mode == "cxxskel":
      functions = [raw_to_cxx_service_skeleton]
      args = [[pargs.interface, pargs.include]]
    elif pargs.output_mode == "cxxservice":
      functions = [raw_to_cxx_service_skeleton, raw_to_cxx_typebuild]
      args = [[pargs.interface, pargs.include], [True]]
    elif pargs.output_mode == "cxxservicebouncer":
      functions = [raw_to_cxx_service_bouncer, raw_to_cxx_typebuild]
      args = [['@Impl', pargs.include], [True]]
    #print("Executing %s functions on %s classes" % (len(functions), len(raw)))
    for c in raw:
      for i in range(len(functions)):
        cargs = [c, raw[c]] + args[i]
        res += functions[i](*cargs)
  out.write(res)

main(sys.argv)
