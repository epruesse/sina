/*
Copyright (c) 2006-2018 Elmar Pruesse <elmar.pruesse@ucdenver.edu>

This file is part of SINA.
SINA is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your
option) any later version.

SINA is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with SINA.  If not, see <http://www.gnu.org/licenses/>.

Additional permission under GNU GPL version 3 section 7

If you modify SINA, or any covered work, by linking or combining it
with components of ARB (or a modified version of that software),
containing parts covered by the terms of the
ARB-public-library-license, the licensors of SINA grant you additional
permission to convey the resulting work. Corresponding Source for a
non-source form of such a combination shall include the source code
for the parts of ARB used as well as that of the covered work.
*/

#include "rw_fasta.h"

#include <fstream>
#include <vector>
#include <iostream>
#include <map>
#include <string>
#include <sstream>

#include <functional>

#include <boost/thread/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>
#include "query_arb.h"

using std::ifstream;
using std::ofstream;
using std::stringstream;
using std::vector;
using std::cerr;
using std::endl;
using std::map;
using std::string;
using std::pair;
using boost::thread;
using boost::shared_ptr;
using boost::bind;
using boost::split;
using boost::algorithm::iequals;

namespace po = boost::program_options;

namespace sina {

// define extra datatype for metadata output format
std::ostream& operator<<(std::ostream& out, const sina::FASTA_META_TYPE& m) {
    switch(m) {
    case FASTA_META_NONE:
        out << "none";
    break;
    case FASTA_META_HEADER:
        out << "header";
    break;
    case FASTA_META_COMMENT:
        out << "comment";
    break;
    case FASTA_META_CSV:
        out << "csv";
    break;
    default:
        out << "[UNKNOWN!] (value=" << (int)m << ")";
    }
    return out;
}
void validate(boost::any& v,
              const std::vector<std::string>& values,
              sina::FASTA_META_TYPE* /*m*/, int) {
    po::validators::check_first_occurrence(v);
    const std::string &s = po::validators::get_single_string(values);
    if (iequals(s, "none")) {
        v = FASTA_META_NONE;
    } else if (iequals(s, "header")) {
        v = FASTA_META_HEADER;
    } else if (iequals(s, "comment")) {
        v = FASTA_META_COMMENT;
    } else if (iequals(s, "csv")) {
        v = FASTA_META_CSV;
    } else {
        throw po::invalid_option_value("must be one of 'none', 'header', 'comment' or 'cvs'");
    }
}

// struct holding configuration options
struct rw_fasta::options {
    FASTA_META_TYPE fastameta;
    int line_length;
    float min_idty;
    long fasta_block;
    long fasta_idx;
    bool out_dots;
    bool out_dna;
};
struct rw_fasta::options *rw_fasta::opts;

void
rw_fasta::get_options_description(po::options_description& main,
                                  po::options_description& adv) {
    opts = new options;

    main.add_options()
        ("meta-fmt",
         po::value<FASTA_META_TYPE>(&opts->fastameta)->default_value(FASTA_META_NONE,""),
         "meta data in (*none*|header|comment|csv)")
        ;

    po::options_description od("Fasta I/O");
    od.add_options()
        // write
        ("line-length",
         po::value<int>(&opts->line_length)->default_value(0, ""),
         "wrap output sequence (unlimited)")

        ("min-idty",
         po::value<float>(&opts->min_idty)->default_value(0.f, ""),
         "only write sequences with align_idty_slv > X, implies calc-idty")
        ("fasta-write-dna",
         po::bool_switch(&opts->out_dna),
         "Write DNA sequences (default: RNA)")
        ("fasta-write-dots",
         po::bool_switch(&opts->out_dots),
         "Use dots instead of dashes to distinguish unknown sequence data from indels")

        // read
        ("fasta-idx",
         po::value<long>(&opts->fasta_idx)->default_value(0, ""),
         "process only sequences beginning in block <arg>")
        ("fasta-block",
         po::value<long>(&opts->fasta_block)->default_value(0, ""),
         "length of blocks")
        ;
    adv.add(od);
}


void
rw_fasta::validate_vm(po::variables_map& /* vm */,
                      po::options_description& /*desc*/) {
}


////////// reader

struct rw_fasta::reader::priv_data {
    std::ifstream in;
    int lineno;
    int seqno;
    priv_data() : lineno(0), seqno(0) {}
    ~priv_data() {
        std::cerr << "FASTA input: " << std::endl
                  << "        sequences: " << seqno-1 << std::endl
                  << "        lines:     " << lineno-1 << std::endl
            ;
    }
};

rw_fasta::reader::reader(const string& infile)
    : data(new priv_data)
{
    data->in.open(infile.c_str(), std::ios_base::in);
    if (data->in.fail()) {
        stringstream msg; 
        msg << "Unable to open file \"" << infile << "\" for reading.";
        throw std::runtime_error(msg.str());
    }
    
    // if fasta blocking enabled, seek to selected block
    if (opts->fasta_block > 0) {
        data->in.seekg(opts->fasta_block * opts->fasta_idx);
    }
}

rw_fasta::reader::reader(const reader& r)
    : data(r.data)
{
}

rw_fasta::reader&
rw_fasta::reader::operator=(const reader& r) {
    data = r.data;
    return *this;
}

rw_fasta::reader::~reader() {
}

bool
rw_fasta::reader::operator()(tray& t) {
    string line;
    t.logstream = new stringstream();
    t.input_sequence = new cseq();
    data->seqno++;
    cseq &c = *t.input_sequence;
    if (data->in.fail()) {
        return false;
    }
    
    // if fasta blocking enabled, check if we've passed block
    // boundary in last sequence
    if (opts->fasta_block > 0
        && data->in.tellg() > opts->fasta_block * (opts->fasta_idx + 1)) {
        return false;
    }

    // skip lines not beginning with '>'
    while (data->in.peek() != '>' && getline(data->in, line).good()) {
        data->lineno++;
    }

    // parse title
    data->lineno++;
    if (getline(data->in, line).good()) {
        if (line[line.size()-1] == '\r') {  // "\r" at end
            line.resize(line.size()-1);
        }
        
        // set name to text between first '>' and first ' '
        unsigned int blank = line.find_first_of(' ');
        if (blank == 0) blank = line.size();
        c.setName(line.substr(1, blank-1));
        if (blank < line.size()) {
            c.set_attr<string>(query_arb::fn_fullname, line.substr(blank+1));
        }
    } else { // didn't get a title
        return false;
    }

    // handle comments
    while (data->in.peek() == ';' && getline(data->in, line).good()) {
        data->lineno++;

        // if the comment contains an attribute: add it. 
        // Otherwise ignore the comment

        size_t equalsign = line.find_first_of("=");
        if (equalsign != string::npos) {
            string key = line.substr(1, equalsign-1);
            boost::trim(key);
            string value = line.substr(equalsign+1);
            boost::trim(value);
            c.set_attr(key,value);
        }
    }

    try {
        // all lines until eof or next /^>/ are data
        while (data->in.peek() != '>' && data->in.good()) {
            getline(data->in, line);
            data->lineno++;
            c.append(line);
        }
    } catch (base_iupac::bad_character_exception& e) {
        std::cerr << "Encountered invalid character while parsing FASTA file." << std::endl
                  << "Line " << data->lineno << " (sequence " << data->seqno << ", '"<< c.getName() << "')"
                  << " contains character '" << e.character << "':" << std::endl
                  << line << std::endl
                  << "SKIPPING SEQUENCE!!!" << std::endl;
        while (data->in.peek() != '>' && getline(data->in, line).good()) {
            data->lineno++;
        }
        delete t.input_sequence;
        return (*this)(t); // FIXME: stack size?
    }

    return true;
}


/////////////// writer

struct rw_fasta::writer::priv_data {
    std::ofstream out;
    std::ofstream out_csv;
    int seqnum;
    int excluded;
    priv_data() : seqnum(0), excluded(0) {}
    ~priv_data() {
        std::cerr << "FASTA output: " << std::endl
                  << "        excluded: " << excluded << std::endl
                  << "        exported: " << seqnum<< std::endl
            ;
    }
};

rw_fasta::writer::writer(const string& outfile)
    : data(new priv_data)
{
    data->out.open(outfile.c_str());
    if (data->out.fail()) {
        stringstream msg; 
        msg << "Unable to open file \"" << outfile << "\" for writing.";
        throw std::runtime_error(msg.str());
    }
    if (opts->fastameta == FASTA_META_CSV) {
        data->out_csv.open((outfile+".csv").c_str());
        if (data->out_csv.fail()) {
            stringstream msg; 
            msg << "Unable to open file \"" << outfile << ".csv\" for writing.";
            throw std::runtime_error(msg.str());
        }
    }
}

rw_fasta::writer::writer(const writer& o)
    : data(o.data)
{
}

rw_fasta::writer&
rw_fasta::writer::operator=(const writer& o) {
    data = o.data;
    return *this;
}

rw_fasta::writer::~writer() {
}

string escape_string(const string& in) {
    if (in.find_first_of("\",\r\n") == string::npos) {
        return in;
    }
    stringstream tmp;
    tmp << "\"";
    size_t j = 0;
    for (size_t i = in.find('"'); i != string::npos; 
         j=i+1, i = in.find('"',i+1)) {
        tmp << in.substr(j, i-j) << "\"\"";
    }
    tmp << in.substr(j) << "\"";
    return tmp.str();
}

tray
rw_fasta::writer::operator()(tray t) {
    if (t.input_sequence == 0) {
        throw std::runtime_error("Received broken tray in " __FILE__);
    }
    if (t.aligned_sequence == 0) {
        std::cerr << "Sequence " << t.input_sequence->getName() 
                  << " was not aligned. Nothing to write to FASTA output."
                  << std::endl;
        ++data->excluded;
        return t;
    }
    if (opts->min_idty > t.aligned_sequence->get_attr<float>(query_arb::fn_idty)) {
        std::cerr << "Sequence " << t.input_sequence->getName() 
                  << " was below idty threshold at " << t.aligned_sequence->get_attr<float>(query_arb::fn_idty) << " and excluded from FASTA output."
                  << std::endl;
        ++data->excluded;
        return t;
    }
    cseq &c = *t.aligned_sequence;

    const std::map<string,cseq::variant>& attrs = c.get_attrs();

    data->out << ">" << c.getName();
    string fname = c.get_attr<string>(query_arb::fn_fullname);
    if (!fname.empty()) {
        data->out << " " << fname;
    }

    switch (opts->fastameta) {
    case FASTA_META_NONE:
        data->out << endl;
        break;
    case FASTA_META_HEADER:
        for (auto& ap: attrs) {
            if (ap.first != query_arb::fn_family
                && ap.first != query_arb::fn_fullname) {
                data->out << " [" << ap.first << "="
                     << boost::apply_visitor(lexical_cast_visitor<string>(),
                                            ap.second)
                     << "]";
            }
        }
        data->out << endl;
        break;
    case FASTA_META_COMMENT:
        data->out << endl;

        for (auto& ap: attrs) {
            if (ap.first != query_arb::fn_family) {
                data->out << "; " << ap.first << "="
                     << boost::apply_visitor(lexical_cast_visitor<string>(),
                                            ap.second)
                     << endl;
            }
        }
        break;
    case FASTA_META_CSV:
        data->out << endl;

        // print header
        if (data->seqnum == 0) {
            data->out_csv << "name";
            for (auto& ap: attrs) {
              if (ap.first != query_arb::fn_family) {
                  data->out_csv << "," << escape_string(ap.first);
              }
            }
            data->out_csv << "\r\n";
        }

        data->out_csv << c.getName();
        for (auto& ap: attrs) {
            if (ap.first != query_arb::fn_family) {
                data->out_csv << ","
                         << escape_string(
                             boost::apply_visitor(
                                 lexical_cast_visitor<string>(),
                                 ap.second
                                 )
                             );
            }
        }

        data->out_csv << "\r\n";
        break;
    default:
        throw std::runtime_error("Unknown meta-fmt output option");
    }

    string seq  = c.getAligned(!opts->out_dots, opts->out_dna);
    int len = seq.size();
    if (opts->line_length > 0) {
        for (int i=0; i<len; i+=opts->line_length) {
            data->out << seq.substr(i, opts->line_length) << endl;
        }
    } else {
        data->out << seq << endl;
    }
    data->seqnum++;

    return t;
}

} // namespace sina

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . 0))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:encoding=utf-8:textwidth=99 :
