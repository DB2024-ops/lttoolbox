/*
 * Copyright (C) 2005 Universitat d'Alacant / Universidad de Alicante
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */
#include <lttoolbox/transducer.h>
#include <lttoolbox/compression.h>

#include <lttoolbox/my_stdio.h>
#include <lttoolbox/lt_locale.h>

#include <cstdlib>
#include <iostream>
#include <libgen.h>
#include <string>
#include <cstring>

void endProgram(char *name)
{
  if(name != NULL)
  {
    cout << basename(name) << " v" << PACKAGE_VERSION <<": trim a transducer to another transducer" << endl;
    cout << "USAGE: " << basename(name) << " analyser_bin_file bidix_bin_file trimmed_bin_file " << endl;
  }
  exit(EXIT_FAILURE);
}

std::pair<std::pair<Alphabet, UString>, std::map<UString, Transducer> >
read_fst(FILE *bin_file)
{
  Alphabet new_alphabet;

  std::map<UString, Transducer> transducers;

  fpos_t pos;
  if (fgetpos(bin_file, &pos) == 0) {
      char header[4]{};
      fread_unlocked(header, 1, 4, bin_file);
      if (strncmp(header, HEADER_LTTOOLBOX, 4) == 0) {
          auto features = read_le<uint64_t>(bin_file);
          if (features >= LTF_UNKNOWN) {
              throw std::runtime_error("FST has features that are unknown to this version of lttoolbox - upgrade!");
          }
      }
      else {
          // Old binary format
          fsetpos(bin_file, &pos);
      }
  }

  // letters
  UString letters = Compression::string_read(bin_file);

  // symbols
  new_alphabet.read(bin_file);

  int len = Compression::multibyte_read(bin_file);

  while(len > 0)
  {
    UString name = Compression::string_read(bin_file);
    transducers[name].read(bin_file);

    len--;
  }

  std::pair<Alphabet, UString> alph_letters;
  alph_letters.first = new_alphabet;
  alph_letters.second = letters;
  return std::pair<std::pair<Alphabet, UString>, std::map<UString, Transducer> > (alph_letters, transducers);
}

std::pair<std::pair<Alphabet, UString>, std::map<UString, Transducer> >
trim(FILE *file_mono, FILE *file_bi)
{
  std::pair<std::pair<Alphabet, UString>, std::map<UString, Transducer> > alph_trans_mono = read_fst(file_mono);
  Alphabet alph_mono = alph_trans_mono.first.first;
  std::map<UString, Transducer> trans_mono = alph_trans_mono.second;
  std::pair<std::pair<Alphabet, UString>, std::map<UString, Transducer> > alph_trans_bi = read_fst(file_bi);
  Alphabet alph_bi = alph_trans_bi.first.first;
  std::map<UString, Transducer> trans_bi = alph_trans_bi.second;

  // The prefix transducer is the union of all transducers from bidix,
  // with a ".*" appended
  Transducer union_transducer;
  // The "." in ".*" is a set of equal pairs of the output symbols
  // from the monodix alphabet (<n>:<n> etc.)
  Alphabet alph_prefix = alph_bi;
  set<int> loopback_symbols;    // ints refer to alph_prefix
  alph_prefix.createLoopbackSymbols(loopback_symbols, alph_mono, Alphabet::right);

  for(std::map<UString, Transducer>::iterator it = trans_bi.begin(); it != trans_bi.end(); it++)
  {
    Transducer union_tmp = it->second;
    if(union_transducer.isEmpty())
    {
      union_transducer = union_tmp;
    }
    else
    {
      union_transducer.unionWith(alph_bi, union_tmp);
    }
  }
  union_transducer.minimize();

  Transducer prefix_transducer = union_transducer.appendDotStar(loopback_symbols);
  // prefix_transducer should _not_ be minimized (both useless and takes forever)
  Transducer moved_transducer = prefix_transducer.moveLemqsLast(alph_prefix);


  for(std::map<UString, Transducer>::iterator it = trans_mono.begin(); it != trans_mono.end(); it++)
  {
    Transducer trimmed = it->second.intersect(moved_transducer,
                                              alph_mono,
                                              alph_prefix);

    cout << it->first << " " << it->second.size();
    cout << " " << it->second.numberOfTransitions() << endl;
    if(it->second.numberOfTransitions() == 0)
    {
      cerr << "Warning: empty section! Skipping it ..."<<endl;
      trans_mono[it->first].clear();
    }
    else if(trimmed.hasNoFinals()) {
      cerr << "Warning: section had no final state after trimming! Skipping it ..."<<endl;
      trans_mono[it->first].clear();
    }
    else {
      trimmed.minimize();
      trans_mono[it->first] = trimmed;
    }
  }

  alph_trans_mono.second = trans_mono;
  return alph_trans_mono;
}


int main(int argc, char *argv[])
{
  if(argc != 4)
  {
    endProgram(argv[0]);
  }

  LtLocale::tryToSetLocale();

  FILE *analyser = fopen(argv[1], "rb");
  if(!analyser)
  {
    cerr << "Error: Cannot open file '" << argv[1] << "'." << endl << endl;
    exit(EXIT_FAILURE);
  }
  FILE *bidix = fopen(argv[2], "rb");
  if(!bidix)
  {
    cerr << "Error: Cannot open file '" << argv[2] << "'." << endl << endl;
    exit(EXIT_FAILURE);
  }

  std::pair<std::pair<Alphabet, UString>, std::map<UString, Transducer> > trimmed = trim(analyser, bidix);
  Alphabet alph_t = trimmed.first.first;
  UString letters = trimmed.first.second;
  std::map<UString, Transducer> trans_t = trimmed.second;

  int n_transducers = 0;
  for(auto& it : trans_t) {
    if(!(it.second.isEmpty()))
    {
      n_transducers++;
    }
  }

  if(n_transducers == 0)
  {
    cerr << "Error: Trimming gave empty transducer!" << endl;
    cerr << "Hint: There are no words in bilingual dictionary that match "
      "words in both monolingual dictionaries?" << endl;
    exit(EXIT_FAILURE);
  }

  // Write the file:
  FILE *output = fopen(argv[3], "wb");
  if(!output)
  {
    cerr << "Error: Cannot open file '" << argv[3] << "'." << endl << endl;
    exit(EXIT_FAILURE);
  }

  // letters
  Compression::string_write(letters, output);

  // symbols
  alph_t.write(output);

  // transducers
  Compression::multibyte_write(n_transducers, output);
  for(auto& it : trans_t) {
    if(!(it.second.isEmpty()))
    {
      Compression::string_write(it.first, output);
      it.second.write(output);
    }
  }

  fclose(analyser);
  fclose(bidix);
  fclose(output);

  return 0;
}
