#!/bin/bash
cd ${NOFEED_AI_DIR:-$HOME/nofeed-ai}/data/ears
for s in p001 p004 p010 p017 p025 p033 p041 p050 p058 p066 p075 p083 p091 p107; do
  [ -d "$s" ] && continue
  for try in 1 2 3; do
    curl -sSL --max-time 1800 -o $s.zip "https://github.com/facebookresearch/ears_dataset/releases/download/dataset/$s.zip" && unzip -q -o $s.zip && rm -f $s.zip && echo "got $s" && break
    sleep 5
  done
done
echo FETCH_DONE
