/*
 * Copyright (C) 2013 Dávid Márk Nemeskey
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

#include <lttoolbox/att_compiler.h>
#include <lttoolbox/alphabet.h>
#include <lttoolbox/transducer.h>
#include <lttoolbox/compression.h>
#include <lttoolbox/string_to_wostream.h>
#include <algorithm>
#include <stack>
#include <unicode/unistr.h>
#include <unicode/numfmt.h>

using namespace std;
using namespace icu;

AttCompiler::AttCompiler() :
starting_state(0),
default_weight(0.0000)
{}

AttCompiler::~AttCompiler()
{}

void
AttCompiler::clear()
{
  for (auto& it : graph)
  {
    delete it.second;
  }
  graph.clear();
  alphabet = Alphabet();
}

/**
 * Converts symbols like @0@ to epsilon, @_SPACE_@ to space, etc.
 * @todo Are there other special symbols? If so, add them, and maybe use a map
 *       for conversion?
 */
void
AttCompiler::convert_hfst(UString& symbol)
{
  if (symbol == (const UChar*)"@0@" || symbol == (const UChar*)"ε")
  {
    symbol = (const UChar*)"";
  }
  else if (symbol == (const UChar*)"@_SPACE_@")
  {
    symbol = (const UChar*)" ";
  }
}

bool
AttCompiler::is_word_punct(UChar symbol)
{
  // https://en.wikipedia.org/wiki/Combining_character#Unicode_ranges
  if((symbol >= 0x0300 && symbol <= 0x036F) // Combining Diacritics
  || (symbol >= 0x1AB0 && symbol <= 0x1AFF) // ... Extended
  || (symbol >= 0x1DC0 && symbol <= 0x1DFF) // ... Supplement
  || (symbol >= 0x20D0 && symbol <= 0x20FF) // ... for Symbols
  || (symbol >= 0xFE20 && symbol <= 0xFE2F)) // Combining Half Marks
  {
    return true;
  }

  return false;
}

/**
 * Returns the code of the symbol in the alphabet. Run after convert_hfst has
 * run.
 *
 * Also adds all non-multicharacter symbols (letters) to the @p letters set.
 *
 * @return the code of the symbol, if @p symbol is multichar; its first (and
 *         only) character otherwise.
 */
int
AttCompiler::symbol_code(const UString& symbol)
{
  if (symbol.length() > 1) {
    alphabet.includeSymbol(symbol);
    return alphabet(symbol);
  } else if (symbol.empty()) {
    return 0;
  } else if ((iswpunct(symbol[0]) || iswspace(symbol[0])) && !is_word_punct(symbol[0])) {
    return symbol[0];
  } else {
    letters.insert(symbol[0]);
    if(iswlower(symbol[0]))
    {
      letters.insert(towupper(symbol[0]));
    }
    else if(iswupper(symbol[0]))
    {
      letters.insert(towlower(symbol[0]));
    }
    return symbol[0];
  }
}

void
AttCompiler::parse(string const &file_name, bool read_rl)
{
  clear();

  UFILE* infile = u_fopen(file_name.c_str(), "r", NULL, NULL);
  if (infile == NULL) {
    cerr << "Error: unable to open '" << file_name << "' for reading." << endl;
  }
  vector<UString> tokens;
  bool first_line_in_fst = true;       // First line -- see below
  bool multiple_transducers = false;
  int state_id_offset = 1;
  int largest_seen_state_id = 0;
  int line_number = 0;

  while (!u_feof(infile))
  {
    line_number++;
    tokens.clear();
    tokens.push_back((UChar*)"");
    do {
      UChar32 c = u_fgetcx(infile);
      if (c == '\n') {
        break;
      } else if (c == '\t') {
        tokens.push_back((UChar*)"");
      } else {
        tokens.back() += c;
      }
    } while (!u_feof(infile));

    int from, to;
    UString upper, lower;
    double weight;

    if (tokens[0].length() == 0 && first_line_in_fst)
    {
      cerr << "Error: empty file '" << file_name << "'." << endl;
      exit(EXIT_FAILURE);
    }
    if (first_line_in_fst && tokens.size() == 1)
    {
      cerr << "Error: invalid format in file '" << file_name << "' on line " << line_number << "." << endl;
      exit(EXIT_FAILURE);
    }

    /* Empty line. */
    if (tokens.size() == 1 && tokens[0].length() == 0)
    {
      continue;
    }

    if (tokens[0].find('-') == 0)
    {
      if (state_id_offset == 1) {
        // this is the first split we've seen
        cerr << "Warning: Multiple fsts in '" << file_name << "' will be disjuncted." << endl;
        multiple_transducers = true;
      }
      // Update the offset for the new FST
      state_id_offset = largest_seen_state_id + 1;
      first_line_in_fst = true;
      continue;
    }

    from = stoi(tokens[0]) + state_id_offset;
    largest_seen_state_id = max(largest_seen_state_id, from);

    AttNode* source = get_node(from);
    /* First line: the initial state is of both types. */
    if (first_line_in_fst)
    {
      AttNode * starting_node = get_node(starting_state);

      // Add an Epsilon transition from the new starting state
      starting_node->transductions.push_back(
         Transduction(from, (const UChar*)"", (const UChar*)"",
                      0, default_weight));
      first_line_in_fst = false;
    }

    /* Final state. */
    if (tokens.size() <= 2)
    {
      if (tokens.size() > 1)
      {
        weight = stod(tokens[1]);
      }
      else
      {
        weight = default_weight;
      }
      finals.insert(pair <int, double>(from, weight));
    }
    else
    {
      to = stoi(tokens[1]) + state_id_offset;
      largest_seen_state_id = max(largest_seen_state_id, to);
      if(read_rl)
      {
        upper = tokens[3];
        lower = tokens[2];
      }
      else
      {
        upper = tokens[2];
        lower = tokens[3];
      }
      convert_hfst(upper);
      convert_hfst(lower);
      int tag = alphabet(symbol_code(upper), symbol_code(lower));
      if(tokens.size() > 4)
      {
        weight = stod(tokens[4]);
      }
      else
      {
        weight = default_weight;
      }
      source->transductions.push_back(Transduction(to, upper, lower, tag, weight));
      classify_single_transition(source->transductions.back());

      get_node(to);
    }
  }

  if (!multiple_transducers) {
    starting_state = 1;
    // if we aren't disjuncting multiple transducers
    // then we have an extra epsilon transduction at the beginning
    // so skip it
  }

  /* Classify the nodes of the graph. */
  classify_forwards();
  set<int> path;
  classify_backwards(starting_state, path);

  u_fclose(infile);
}

/** Extracts the sub-transducer made of states of type @p type. */
Transducer
AttCompiler::extract_transducer(TransducerType type)
{
  Transducer transducer;
  /* Correlation between the graph's state ids and those in the transducer. */
  map<int, int> corr;
  set<int> visited;

  corr[starting_state] = transducer.getInitial();
  _extract_transducer(type, starting_state, transducer, corr, visited);

  /* The final states. */
  bool noFinals = true;
  for (auto& f : finals)
  {
    if (corr.find(f.first) != corr.end())
    {
      transducer.setFinal(corr[f.first], f.second);
      noFinals = false;
    }
  }

/*
  if(noFinals)
  {
    wcerr << L"No final states (" << type << ")" << endl;
    wcerr << L"  were:" << endl;
    wcerr << L"\t" ;
    for (auto& f : finals)
    {
      wcerr << f.first << L" ";
    }
    wcerr << endl;
  }
*/
  return transducer;
}

/**
 * Recursively fills @p transducer (and @p corr) -- helper method called by
 * extract_transducer().
 */
void
AttCompiler::_extract_transducer(TransducerType type, int from,
                                 Transducer& transducer, map<int, int>& corr,
                                 set<int>& visited)
{
  if (visited.find(from) != visited.end())
  {
    return;
  }
  else
  {
    visited.insert(from);
  }

  AttNode* source = get_node(from);

  /* Is the source state new? */
  bool new_from = corr.find(from) == corr.end();
  int from_t, to_t;

  for (auto& it : source->transductions)
  {
    if ((it.type & type) != type)
    {
      continue;  // Not the right type
    }
    /* Is the target state new? */
    bool new_to = corr.find(it.to) == corr.end();

    if (new_from)
    {
      corr[from] = transducer.size() + (new_to ? 1 : 0);
    }
    from_t = corr[from];

    /* Now with the target state: */
    if (!new_to)
    {
      /* We already know it, possibly by a different name: link them! */
      to_t = corr[it.to];
      transducer.linkStates(from_t, to_t, it.tag, it.weight);
    }
    else
    {
      /* We haven't seen it yet: add a new state! */
      to_t = transducer.insertNewSingleTransduction(it.tag, from_t, it.weight);
      corr[it.to] = to_t;
    }
    _extract_transducer(type, it.to, transducer, corr, visited);
  }  // for
}

void
AttCompiler::classify_single_transition(Transduction& t)
{
  if (t.upper.length() == 1) {
    if (letters.find(t.upper[0]) != letters.end()) {
      t.type |= WORD;
    }
    if (iswpunct(t.upper[0])) {
      t.type |= PUNCT;
    }
  }
}

/**
 * Propagate edge types forwards.
 */
void
AttCompiler::classify_forwards()
{
  stack<int> todo;
  set<int> done;
  todo.push(starting_state);
  while(!todo.empty()) {
    int next = todo.top();
    todo.pop();
    if(done.find(next) != done.end()) continue;
    AttNode* n1 = get_node(next);
    for(auto& t1 : n1->transductions) {
      AttNode* n2 = get_node(t1.to);
      for(auto& t2 : n2->transductions) {
	t2.type |= t1.type;
      }
      if(done.find(t1.to) == done.end()) {
	todo.push(t1.to);
      }
    }
    done.insert(next);
  }
}

/**
 * Recursively determine edge types of initial epsilon transitions
 * Also check for epsilon loops or epsilon transitions to final states
 * @param state the state to examine
 * @param path the path we took to get here
 */
TransducerType
AttCompiler::classify_backwards(int state, set<int>& path)
{
  if(finals.find(state) != finals.end()) {
    wcerr << L"ERROR: Transducer contains epsilon transition to a final state. Aborting." << endl;
    exit(EXIT_FAILURE);
  }
  AttNode* node = get_node(state);
  TransducerType type = UNDECIDED;
  for(auto& t1 : node->transductions) {
    if(t1.type != UNDECIDED) {
      type |= t1.type;
    } else if(path.find(t1.to) != path.end()) {
      wcerr << L"ERROR: Transducer contains initial epsilon loop. Aborting." << endl;
      exit(EXIT_FAILURE);
    } else {
      path.insert(t1.to);
      t1.type = classify_backwards(t1.to, path);
      type |= t1.type;
      path.erase(t1.to);
    }
  }
  // Note: if type is still UNDECIDED at this point, then we have a dead-end
  // path, which is fine since it will be discarded by _extract_transducer()
  return type;
}


/** Writes the transducer to @p file_name in lt binary format. */
void
AttCompiler::write(FILE *output)
{
//  FILE* output = fopen(file_name, "wb");
  fwrite(HEADER_LTTOOLBOX, 1, 4, output);
  uint64_t features = 0;
  write_le(output, features);

  Transducer punct_fst = extract_transducer(PUNCT);

  /* Non-multichar symbols. */
  Compression::string_write(UString(letters.begin(), letters.end()), output);
  /* Multichar symbols. */
  alphabet.write(output);
  /* And now the FST. */
  if(punct_fst.numberOfTransitions() == 0)
  {
    Compression::multibyte_write(1, output);
  }
  else
  {
    Compression::multibyte_write(2, output);
  }
  Compression::string_write((const UChar*)"main@standard", output);
  Transducer word_fst = extract_transducer(WORD);
  word_fst.write(output);
  wcout << L"main@standard" << " " << word_fst.size();
  wcout << " " << word_fst.numberOfTransitions() << endl;
  Compression::string_write((const UChar*)"final@inconditional", output);
  if(punct_fst.numberOfTransitions() != 0)
  {
    punct_fst.write(output);
    wcout << L"final@inconditional" << " " << punct_fst.size();
    wcout << " " << punct_fst.numberOfTransitions() << endl;
  }
//  fclose(output);
}
