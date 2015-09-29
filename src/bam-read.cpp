#include "bam-read.h"

int BamRead::estimate_fragment_size(std::string file) {
  if (!reader.Open(file)) {
    cerr << "Could not open BAM file" << endl;
    return 1;
  }
  int count = 0;
  std::string name = "";
  std::string prev = "";
  int pos1 = -1;
  int pos2 = -1;
  int len1 = -1;
  int len2 = -1;
  int fragment = 0; // mean fragment length
  int mean = 0;
  double mn = -1;
  double m = -1;
  double s = 0;
  bool is_reversed;
  bool is_mate_reversed;

  while (reader.GetNextAlignment(alignment) && count < 10000) {
    if (name != "") {
      prev = name;
      pos2 = pos1;
      len2 = len1;
    }
    if (alignment.IsPrimaryAlignment()) { // does this do anything?
      name = alignment.Name;
      pos1 = alignment.Position;
      len1 = alignment.Length;

      if (prev == name) {
        if (pos1 >= 0 && pos2 >= 0) {
          is_reversed = alignment.IsReverseStrand();
          is_mate_reversed = alignment.IsMateReverseStrand();

          if (!is_reversed && is_mate_reversed) {
            if (pos1 > alignment.MatePosition) {
              continue;
            }
          } else if (is_reversed && !is_mate_reversed) {
            if (alignment.MatePosition > pos1) {
              continue;
            }
          }

          if (pos1 > pos2) {
            fragment = (pos1 - pos2 + len1);
          } else {
            fragment = (pos2 - pos1 + len2);
          }
          if (count > 0) {
            mn = m + (fragment - m)/count;
            s = s + ((fragment - m) * (fragment - mn));
            m = mn;
          } else {
            m = fragment;
          }
          mean += fragment;
          ++count;
        }
      }
    }
  }
  mean=mean/(double)count;
  // cout << "count: " << count << " mean:" << mean << endl;
  s = sqrt(s/(count-1));
  // cout << "sd: " << s << endl;
  realistic_distance = (int)(3 * s + mean);
  return 0;
}

int BamRead::load_bam(std::string file) {
  if (!reader.Open(file)) {
    cerr << "Could not open BAM file" << endl;
    return 1;
  }

  // load the sam header
  SamSequenceDictionary dictionary = reader.GetHeader().Sequences;
  seq_count = dictionary.Size();

  array.resize(seq_count);

  // fill the hash map with initial values
  std::vector<SamSequence>::iterator it = dictionary.Begin();
  int len = 0;
  for (int i = 0; i < seq_count; i++) {
    if (it[i].HasLength()) {
      len = atoi(it[i].Length.c_str());
    } else {
      len = 0;
    }
    // array[i] = TransratePileup(it[i].Name, len);
    array[i].setName(it[i].Name);
    array[i].setLength(len);
  }

  // loop through the bam file
  double scale = 0.65;
  double seq_true = 0;
  while (reader.GetNextAlignment(alignment)) {
    // read must be mapped
    if (!alignment.IsMapped()) {
      continue;
    }
    refid = alignment.RefID;
    ++array[refid].reads_mapped;

    array[refid].addAlignment(alignment);

    // store edit distance for sequence accuracy calculation
    if (alignment.HasTag("NM")) {
      if (alignment.GetTag("NM", nm_tag)) {
        len = alignment.Length;
        scale = (len - 35)/(double)len;
        seq_true = (len - nm_tag)/(double)len;
        seq_true = (seq_true - scale) * (1 / (1 - scale));
        array[refid].p_seq_true += seq_true;
      }
    }

    // count fragments where either or both mates mapped
    if (alignment.IsFirstMate() ||
      (alignment.IsSecondMate() && !alignment.IsMateMapped())) {
      ++array[refid].fragments_mapped;
    }

    // from now on ignore fragments unless both mates mapped
    if (!(alignment.IsFirstMate() && alignment.IsMateMapped())) {
      continue;
    }

    ++array[refid].both_mapped;

    // // count proper pairs, although we have our own definition because
    // // not all aligners use the same definition
    if (alignment.IsProperPair()) {
      ++array[refid].properpair;
    }

    // // mates must align to same contig, otherwise we record a bridge
    if (refid != alignment.MateRefID) {
      ++array[refid].bridges;
      continue;
    }

    ldist = max(alignment.Position-alignment.MatePosition,
                alignment.MatePosition-alignment.Position);
    if (ldist > realistic_distance) {
      // mates are too far apart
      continue;
    }

    // read orientation must match the generated library
    // in this case we only test for FR/RF orientation,
    // that is - we expect mates to be on opposite strands
    bool is_reversed = alignment.IsReverseStrand();
    bool is_mate_reversed = alignment.IsMateReverseStrand();

    if (!is_reversed && is_mate_reversed) {
      // in FR orientation, first read must start
      // before second read
      if (alignment.Position < alignment.MatePosition) {
        array[refid].good++;
      }
    } else if (is_reversed && !is_mate_reversed) {
      // in RF orientation, second read must start
      // before first read
      if (alignment.MatePosition < alignment.Position) {
        array[refid].good++;
      }
    }
  } // end of bam file
  for (int i = 0; i < seq_count; i++) {
    array[i].calculateUncoveredBases();
    array[i].setPNotSegmented();
  }
  return 0;
}

int main (int argc, char* argv[]) {
  BamRead bam;

  if (argc >= 3) {
    if (argc == 4) {
      // user has supplied a segmentation null prior
      nullprior = atof(argv[3]);
    }
    string infile = argv[1];
    bam.estimate_fragment_size(infile);
    bam.load_bam(infile);
    // open file for writing
    std::ofstream output;
    output.open (argv[2]);
    output << "name,p_seq_true,bridges,length,fragments_mapped,"
              "both_mapped,properpair,good,bases_uncovered,"
              "p_not_segmented" << endl;

    for (int i = 0; i < bam.seq_count; i++) {
      output << bam.array[i].name << ",";
      if (bam.array[i].reads_mapped > 0) {
        output << bam.array[i].p_seq_true/bam.array[i].reads_mapped << ",";
      } else {
        output << "1,";
      }
      output << bam.array[i].bridges << ",";
      output << bam.array[i].ref_length << ",";
      output << bam.array[i].fragments_mapped << ",";
      output << bam.array[i].both_mapped << ",";
      output << bam.array[i].properpair << ",";
      output << bam.array[i].good << ",";
      output << bam.array[i].bases_uncovered << ",";
      output << bam.array[i].p_not_segmented << endl;
    }

    output.close();

    return 0;
  } else {
    cout << "bam-read version 1.0.0\n"
         << "Usage:\n"
         << "bam-read <bam_file> <output_csv> <nullprior (optional)>\n\n"
         << "example:\n"
         << "bam-read in.bam out.csv 0.95"
         << endl;
    return 1;
  }
}
