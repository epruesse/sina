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

#include "rw_csv.h"
#include "log.h"
#include "query_arb.h"

#include <boost/program_options.hpp>
namespace po = boost::program_options;
#include <boost/filesystem.hpp>
namespace fs = boost::filesystem;
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
namespace bi = boost::iostreams;
#include <boost/algorithm/string.hpp>
using boost::algorithm::equals;

namespace sina {
namespace rw_csv {

static const char* module_name = "CSV I/O";
static auto logger = Log::create_logger(module_name);

struct options {
    bool crlf;
};
static options opts;

void get_options_description(po::options_description &main,
                             po::options_description &adv) {
    po::options_description od(module_name);
    od.add_options()
        ("csv-crlf", po::bool_switch(&opts.crlf),
         "Write CSV using CRLF line ends (as RFC4180 demands)");
    adv.add(od);
}

void validate_vm(po::variables_map &, po::options_description &) {
}

struct writer::priv_data {
    bi::file_descriptor_sink file;
    bi::filtering_ostream out;
    unsigned long copy_relatives;
    std::vector<std::string> v_fields;
    std::vector<std::string> headers;
    bool header_printed{false};
    const char *line_end;
    size_t line_end_len;

    void add_newline(fmt::memory_buffer& buf) {
        buf.append(line_end, line_end + line_end_len);
    }
};

writer::writer(const fs::path& outfile, unsigned int copy_relatives,
               std::vector<std::string>& fields)
    : data(new priv_data())
{
    data->copy_relatives = copy_relatives;
    data->v_fields = fields;

    if (outfile == "-") {
        data->file.open(STDOUT_FILENO, bi::never_close_handle);
    } else {
        data->file.open(outfile.c_str(), std::ios_base::binary);
    }
    if (!data->file.is_open()) {
        auto msg = "Unable to open file {} for writing.";
        throw std::runtime_error(fmt::format(msg, outfile));
    }
    if (outfile.extension() == ".gz") {
        data->out.push(bi::gzip_compressor());
    }
    data->out.push(data->file);

    if (opts.crlf) {
        data->line_end = "\r\n";
        data->line_end_len = 2;
    } else {
        data->line_end = "\n";
        data->line_end_len = 1;
    }
}
writer::writer(const writer&) = default;
writer& writer::operator=(const writer&) = default;
writer::~writer() = default;


static inline void append(fmt::memory_buffer& buf, const std::string& str) {
    if (str.find_first_of("\",\r\n") == std::string::npos) {
        buf.append(str.data(), str.data() + str.size());
    } else {
        const char quote[] = "\"";
        buf.append(quote, quote + sizeof(quote) - 1);
        size_t j = 0;
        for (auto i = str.find('"'); i != std::string::npos; i = str.find('"', i+1)) {
            buf.append(str.data() + j, str.data() + i);
            buf.append(quote, quote + sizeof(quote) - 1);
            j = i;
        }
        buf.append(str.data() + j, str.data() + str.size());
        buf.append(quote, quote + sizeof(quote) - 1);
    }
}


tray writer::operator()(tray t) {
    const char sep[] = ",";
    const char id[] = "name";
    fmt::memory_buffer buf;

    if (t.aligned_sequence == nullptr) {
        return t;
    }
    if (!data->header_printed) {
        if (data->v_fields.empty() ||
            (data->v_fields.size() == 1 &&
             equals(data->v_fields[0], query_arb::fn_fullname))
            ) {
            auto attrs = t.aligned_sequence->get_attrs();
            data->headers.reserve(attrs.size());
            for (auto& ap : attrs) {
                data->headers.push_back(ap.first);
            }
        } else {
            data->headers.reserve(data->v_fields.size());
            for (const auto& f: data->v_fields) {
                data->headers.push_back(f);
            }
        }
        buf.append(id, id + sizeof(id)-1);
        for (const auto& header : data->headers) {
            buf.append(sep, sep + sizeof(sep)-1);
            append(buf, header);
        }
        data->add_newline(buf);
        data->header_printed = true;
    }

    const std::string& name = t.aligned_sequence->getName();
    append(buf, name);
    for (const auto& key : data->headers) {
        buf.append(sep, sep + sizeof(sep)-1);
        append(buf, t.aligned_sequence->get_attr<std::string>(key));
    }
    data->add_newline(buf);

    fmt::internal::write(data->out, buf);

    return t;
}

} // namespace rw_csv
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
