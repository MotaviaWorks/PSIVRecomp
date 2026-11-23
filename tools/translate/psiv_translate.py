#!/usr/bin/env python3
"""PSIV Recomp: the translators' tool.

Reads the dialogue out of YOUR Phantasy Star IV (USA) ROM into a workbook (a .csv any spreadsheet
opens), checks a translation written into it, and writes the pack to share: your translation alone,
by line number, with none of the game's own words in it.

  python psiv_translate.py extract ROM [-o psiv-dialogue.csv]
  python psiv_translate.py check   WORKBOOK [--rom ROM]
  python psiv_translate.py pack    WORKBOOK --language NAME [-o psiv-NAME.txt] [--rom ROM]

Python 3.8 or later, nothing else. README.md beside this file is the translators' guide.

How the game keeps its dialogue: 43 banks, each compressed (Kosinski) in the ROM and unpacked whole
into memory when its part of the game loads. A bank is a run of lines, each ended by $FF and found by
its number. A line is letters (the dialogue's own character codes) and control codes $F0-$FF, some
followed by bytes of their own:
  $F2 an action (panels, sounds, flags)   $F4 a portrait          $F5 a yes / no choice (two lines)
  $F6 an event                            $F7 $FE ends            $F9 a pause
  $FA a line to jump to if a flag is set  $FC a new line          $FD the page ends: wait for a button
"""
import argparse
import csv
import hashlib
import io
import re
import sys
import unicodedata

ROM_SHA1 = 'bc7ff6d6a8408f38562bc610f24645cad6c42629'      # Phantasy Star IV (USA), as a plain .bin
# where each bank starts in that ROM (DialogueTree1 .. DialogueTree43)
BANKS = [0x1DF600, 0x1E0190, 0x1E0BA0, 0x1E1980, 0x1E2680, 0x1E32A0, 0x1E3F40, 0x1E4E20, 0x1E5740,
         0x1E6380, 0x1E6FE0, 0x1E7D50, 0x1E89E0, 0x1E99F0, 0x1EA8C0, 0x1EB1A0, 0x1EBA90, 0x1EC870,
         0x1ED360, 0x1EDCA0, 0x1EE9B0, 0x1EF130, 0x1EFA80, 0x1F0620, 0x1F10C0, 0x1F1AD0, 0x1F2400,
         0x1F2DF0, 0x1F3A90, 0x1F4570, 0x1F4F70, 0x1F5A70, 0x1F6320, 0x1F7190, 0x1F81B0, 0x1F9020,
         0x1F9580, 0x1FA480, 0x1FAAC0, 0x1FB610, 0x1FC3B0, 0x1FC920, 0x1FD7A0]
# The portrait code has a second byte in the banks spoken from the camp menu's Talk (the game reads
# it only there): banks 31 and 32.
TWO_BYTE_PORTRAIT = {31, 32}
ACTION_BYTES = {0x00: 2, 0x03: 1, 0x04: 1, 0x0B: 1}        # the bytes an action takes after its number
PORTRAITS = {1: 'Chaz', 2: 'Alys', 3: 'Hahn', 4: 'Rune', 5: 'Gryz', 6: 'Rika', 7: 'Demi', 8: 'Wren',
             9: 'Raja', 0xA: 'Kyra', 0xB: 'Seth', 0xC: 'Saya', 0xD: 'Holt', 0xE: 'the principal',
             0xF: 'Dorin', 0x10: 'Pana', 0x11: "the Hunters' Guild receptionist", 0x12: 'the baker',
             0x13: 'Zio', 0x14: 'Juza', 0x15: 'Gyuna', 0x16: 'an Esper', 0x17: 'an Esper',
             0x18: 'the Esper chief', 0x19: 'the Esper chief', 0x1A: 'a Gumbious priest',
             0x1B: 'the Gumbious bishop', 0x1C: 'Lashiec', 0x1D: 'Xe-A-Thoul', 0x1E: 'Xe-A-Thoul',
             0x1F: 'Xe-A-Thoul', 0x20: 'the fortune teller', 0x21: 'D-Elm-Lars', 0x22: 'Alys (wounded)',
             0x23: 'Re-Faze', 0x24: 'the missing student', 0x25: 'Tallas', 0x26: 'the dying boy',
             0x27: 'Sekreas'}

# the dialogue's character codes (the game's script/charset)
CHARS = {0: ' ', 0x35: '.', 0x36: "'", 0x37: ',', 0x38: '·', 0x39: ':', 0x3A: '!', 0x3B: '?',
         0x3C: '-', 0x3D: '“', 0x3E: '”', 0x3F: '%'}
CHARS.update({1 + i: chr(ord('A') + i) for i in range(26)})
CHARS.update({27 + i: chr(ord('a') + i) for i in range(26)})
CHARS.update({64 + i: chr(ord('0') + i) for i in range(10)})

# where in the game each bank is heard (for the workbook's "where" column)
WHERE = ['Piata: the Academy, the inn, the shops', 'Piata: the town, the dormitory', 'Mile, Birth Valley',
         'Mile, Zema', 'Krup', 'story scenes (Krup, the Psycho-Wand)', 'Molcum, Tonoe, the valley maze',
         'Nalya, the wreckage', 'Kadary', 'Aiedo: its houses and shops', 'Aiedo: the town, the prison',
         'Monsen', "Termi, Zio's fort, the Plate System", "Tyler, Raja's temple, the hangar", 'Ryuon',
         "Ryuon's pub", 'the Climate Center, Zosa, Myst Vale', 'Meese: its houses and shops', 'Meese, Reshel',
         "the Espers' mansion: Elsydeon's cave, the Inner Sanctuary", "the Espers' mansion", 'the Air Castle',
         'Jut', 'Torinco', "Uzo, the Rappies' cave", "the Hunters' Guild", "the Hunters' Guild: its requests",
         'things looked at (1)', 'things looked at (2)', 'things looked at (3)', "the camp menu's Talk (1)",
         "the camp menu's Talk (2)", "the Academy's basement, the principal's office",
         'Ladea Tower, the Bio-Plant', 'Nurvus, Zelan, Kuran', 'Nurvus: Zio', 'Gumbious: story scenes',
         'Gumbious', 'the spaceports, Rykros: story scenes', "Le Roof's room, the Tower of Courage",
         'the Towers of Strength, Courage and Anger',
         "the ending, the Soldier's Temple, the Edge, the island's cave",
         'the world maps, the Weapon Plant, Vahal Fort']

# The windows' character codes (names, menus, messages): pairs of a code in hex and its character.
WIN_CHARS = {int(m[:2], 16): m[2] for m in re.findall(r'[0-9A-F]{2}.', "00 01A02B03C04D05E06F07G08H09I0AJ0BK0CL0DM0EN0FO10P11Q12R13S14T15U16V17W18X19Y1AZ1B01C11D21E31F420521623831-32!33?34:37,39a3Ab3Bc3Cd3De3Ef3Fg40h41i42j43k44l45m46n47o48p49q4Ar4Bs4Ct4Du4Ev4Fw50x51y52z53.54'77/80A81B82C83D84E86G87H88I8AK8BL8CM8DN8EO8FP91R92S93T94U96W97X98YA51A62A84B3:")}
# The game's short text: a heading, its character set (0 the windows', 1 the dialogue's) and its entries,
# each an address and the length of each of its lines (the lines stand one byte, a new line, apart).
# Numbers only: the words are read from the ROM.
SHORT_TABLES = """\
Enemy names|0|280D42.5;280D48.A;280D53.A;280D5E.A;280D69.A;280D74.A;280D7F.A;280D8A.6;280D91.7;280D99.A;280DA4.A;280DAF.A;280DBA.9;280DC4.A;280DCF.7;280DD7.7;280DDF.A;280DEA.5;280DF0.7;280DF8.7;280E00.7;280E08.7;280E10.A;280E1B.9;280E25.A;280E30.8;280E39.A;280E44.7;280E4C.A;280E57.A;280E62.7;280E6A.A;280E75.A;280E80.4;280E85.8;280E8E.7;280E96.9;280EA0.9;280EAA.A;280EB5.5;280EBB.A;280EC6.9;280ED0.A;280EDB.A;280EE6.9;280EF0.A;280EFB.A;280F06.9;280F10.8;280F19.A;280F24.A;280F2F.6;280F36.8;280F3F.9;280F49.7;280F51.6;280F58.9;280F62.A;280F6D.A;280F78.A;280F83.A;280F8E.A;280F99.8;280FA2.8;280FAB.A;280FB6.A;280FC1.A;280FCC.9;280FD6.6;280FDD.9;280FE7.A;280FF2.A;280FFD.A;281008.A;281013.A;28101E.A;281029.A;281034.A;28103F.9;281049.8;281052.9;28105C.A;281067.9;281071.6;281078.A;281083.7;28108B.9;281095.8;28109E.A;2810A9.8;2810B2.6;2810B9.8;2810C2.7;2810CA.9;2810D4.A;2810DF.9;2810E9.7;2810F1.A;2810FC.A;281107.9;281111.A;28111C.A;281127.6;28112E.A;281139.6;281140.A;28114B.5;281151.7;281159.7;281161.6;281168.5;28116E.A;281179.A;281184.A;28118F.4;281194.8;28119D.6;2811A4.A;2811AF.9;2811B9.A;2811C4.7;2811CC.7;2811D4.A;2811DF.A;2811EA.A;2811F5.A;281200.4;281205.7;28120D.7;281215.7;28121D.7;281225.7;28122D.7;281235.7;28123D.7;281245.7;28124D.9;281257.A;281262.A;28126D.3;281271.3;281275.8;28127E.8;281287.8;281290.9;28129A.8;2812A3.A;2812AE.5;2812B4.A;2812BF.A;2812CA.A;2812D5.A;2812E0.3
Enemy attacks|0|2812E4.7;2812EC.A;2812F7.8;281300.A;28130B.9;281315.7;28131D.7;281325.A;281330.A;28133B.9;281345.A;281350.7;281358.7;281360.A;28136B.A;281376.6;28137D.6;281384.6;28138B.A;281396.7;28139E.A;2813A9.5;2813AF.7;2813B7.9;2813C1.A;2813CC.A;2813D7.7;2813DF.A;2813EA.7;2813F2.5;2813F8.9;281402.A;28140D.A;281418.A;281423.A;28142E.A;281439.9;281443.5;281449.5;28144F.5;281455.5;28145B.5;281461.6;281468.8;281471.5;281477.5;28147D.3;281481.A;28148C.3;281490.5;281496.A;2814A1.5;2814A7.5;2814AD.A;2814B8.A;2814C3.A;2814CE.9;2814D8.7;2814E0.7;2814E8.A;2814F3.A;2814FE.5;281504.A;28150F.3;281513.7;28151B.9;281525.A;281530.3;281534.3;281538.3;28153C.3;281540.5;281546.5;28154C.9;281556.A;281561.8;28156A.8;281573.8;28157C.4;281581.9;28158B.8;281594.6;28159B.9;2815A5.A;2815B0.6;2815B7.A;2815C2.5;2815C8.A;2815D3.A;2815DE.6;2815E5.6;2815EC.A;2815F7.6;2815FE.5;281604.A;28160F.9;281619.A;281624.9;28162E.8;281637.A;281642.A;28164D.A;281658.7;281660.7;281668.9;281672.A;28167D.A;281688.A;281693.A;28169E.A;2816A9.7;2816B1.A
Combination and vehicle attacks|0|28554C.A;285557.A;285562.8;28556B.A;285576.A;285581.A;28558C.A;285597.A;2855A2.9;2855AC.A;2855B7.A;2855C2.A;2855CD.A;2855D8.8;285630.7;285638.7;285640.7;285648.7;285650.6;285657.7;28565F.7;285667.7
Techniques|0|2AB622.3;2AB626.5;2AB62C.5;2AB632.3;2AB636.5;2AB63C.5;2AB642.3;2AB646.5;2AB64C.5;2AB652.3;2AB656.5;2AB65C.5;2AB662.3;2AB666.5;2AB66C.5;2AB672.5;2AB678.5;2AB67E.3;2AB682.5;2AB688.5;2AB68E.5;2AB694.5;2AB69A.5;2AB6A0.3;2AB6A4.5;2AB6AA.5;2AB6B0.3;2AB6B4.5;2AB6BA.5;2AB6C0.5;2AB6C6.5;2AB6CC.5;2AB6D2.5;2AB6D8.4;2AB6DD.5;2AB6E3.5;2AB6E9.5;2AB6EF.5;2AB6F5.5;2AB6FB.5
Skills|0|2AB702.8;2AB70B.8;2AB714.8;2AB71D.6;2AB724.5;2AB72A.6;2AB731.6;2AB738.8;2AB741.7;2AB749.4;2AB74E.6;2AB755.5;2AB75B.6;2AB762.8;2AB76B.8;2AB774.8;2AB77D.6;2AB784.8;2AB78D.8;2AB796.7;2AB79E.8;2AB7A7.4;2AB7AC.5;2AB7B2.5;2AB7B8.8;2AB7C1.8;2AB7CA.7;2AB7D2.8;2AB7DB.6;2AB7E2.6;2AB7E9.5;2AB7EF.8;2AB7F8.8;2AB801.5;2AB807.8;2AB810.6;2AB817.8;2AB820.7;2AB828.7;2AB830.8;2AB839.5;2AB83F.7;2AB847.6;2AB84E.7;2AB856.8;2AB85F.8;2AB868.6;2AB86F.8;2AB878.6;2AB87F.4;2AB884.6;2AB88B.5;2AB891.7;2AB899.7
Battle messages|0|27E5B0.8;27E5BA.8;27E5C3.4;27E5C8.8;27E5D4.10;27E5E5.10;27E5F6.10;27E607.10;27E619.F;27E629.E;27E638.E;27E647.F;27E657.E;27E666.B;27E672.9;27E688.A;27E693.B;27E6A3.F;27E6B7.10;27E6C8.C;27E6D8.E;27E6E9.5;27E6F0.5;27E6F7.4;27E6FC.10;27E70E.7;27E716.B;27E722.E;27E732.D;27E74A.B;27E756.6;27E75E.5;27E764.5;27E76A.3;27E76E.7;27E776.5;27E77C.7;27E784.12;27E798.10;27E7AA.10;27E7BE.3;27E7C2.2;27E7C6.9;27E7D0.8;27E7D9.5;27E7DF.6;27E7E6.3;27E7EA.3;27E7EE.E;27E7FE.10;27E810.E;27E820.F;27E830.11;27E842.F;27E852.F;27E862.A;27E86E.6;27E876.12
Item names|0|2AAFB4.6;2AAFBB.A;2AAFC6.9;2AAFD0.A;2AAFDB.9;2AAFE5.A;2AAFF0.9;2AAFFA.A;2AB005.7;2AB00D.A;2AB018.9;2AB022.A;2AB02D.9;2AB037.A;2AB042.7;2AB04A.9;2AB054.A;2AB05F.A;2AB06A.A;2AB075.A;2AB080.8;2AB089.9;2AB093.9;2AB09D.A;2AB0A8.9;2AB0B2.A;2AB0BD.7;2AB0C5.9;2AB0CF.A;2AB0DA.A;2AB0E5.A;2AB0F0.4;2AB0F5.A;2AB100.A;2AB10B.A;2AB116.A;2AB121.A;2AB12C.9;2AB136.9;2AB140.A;2AB14B.9;2AB155.A;2AB160.8;2AB169.A;2AB174.9;2AB17E.9;2AB188.8;2AB191.A;2AB19C.A;2AB1A7.9;2AB1B1.8;2AB1BA.A;2AB1C5.9;2AB1CF.A;2AB1DA.A;2AB1E5.8;2AB1EE.A;2AB1F9.A;2AB204.9;2AB20E.A;2AB219.9;2AB223.A;2AB22E.A;2AB239.A;2AB244.9;2AB24E.A;2AB259.A;2AB264.8;2AB26D.A;2AB278.A;2AB283.9;2AB28D.A;2AB298.9;2AB2A2.A;2AB2AD.9;2AB2B7.A;2AB2C2.A;2AB2CD.A;2AB2D8.A;2AB2E3.A;2AB2EE.9;2AB2F8.A;2AB303.A;2AB30E.A;2AB319.A;2AB324.A;2AB32F.9;2AB339.A;2AB344.9;2AB34E.A;2AB359.A;2AB364.9;2AB36E.A;2AB379.9;2AB383.A;2AB38E.A;2AB399.9;2AB3A3.8;2AB3AC.A;2AB3B7.A;2AB3C2.A;2AB3CD.A;2AB3D8.9;2AB3E2.9;2AB3EC.A;2AB3F7.A;2AB402.A;2AB40D.A;2AB418.A;2AB423.A;2AB42E.A;2AB439.9;2AB443.A;2AB44E.A;2AB459.A;2AB464.A;2AB46F.A;2AB47A.7;2AB482.8;2AB48B.8;2AB494.A;2AB49F.A;2AB4AA.7;2AB4B2.A;2AB4BD.8;2AB4C6.6;2AB4CD.7;2AB4D5.8;2AB4DE.A;2AB4E9.8;2AB4F2.8;2AB4FB.8;2AB504.8;2AB50D.7;2AB515.A;2AB520.A;2AB52B.A;2AB536.A;2AB541.8;2AB54A.7;2AB552.8;2AB55B.A;2AB566.A;2AB571.A;2AB57C.9;2AB586.A;2AB591.A;2AB59C.7;2AB5A4.A;2AB5AF.A;2AB5BA.A;2AB5C5.9;2AB5CF.A;2AB5DA.9;2AB5E4.A;2AB5EF.9;2AB5F9.9;2AB603.9;2AB60D.9;2AB617.A
Item descriptions|1|2AC944.14+6;2AC960.15+6;2AC97D.10;2AC98E.15+17;2AC9BC.17+13;2AC9E8.18+F;2ACA11.18+F;2ACA3A.16;2ACA51.16+7;2ACA70.15+A;2ACA91.17+16;2ACAC0.17+18;2ACAF1.10+15;2ACB18.15+18;2ACB47.15+18;2ACB76.14+16;2ACBA2.17+15;2ACBD0.11+F;2ACBF2.12+F;2ACC15.13+F;2ACC39.10+F;2ACC5A.16+F;2ACC81.F+F;2ACCA1.12+F;2ACCC4.12+F;2ACCE7.18+11;2ACD13.17+18;2ACD44.18+10;2ACD6E.18;2ACD87.18+13;2ACDB4.16+18;2ACDE4.18;2ACDFD.10+8;2ACE17.18+16;2ACE47.16+16;2ACE75.15+18;2ACEA4.16+14;2ACED0.10+8;2ACEEA.17+16;2ACF19.17+16;2ACF48.14+E;2ACF6C.11+16;2ACF95.13+16;2ACFC0.15+E;2ACFE5.17+14;2AD012.17+16;2AD041.14+16;2AD06D.12+10;2AD091.15+16;2AD0BE.17+10;2AD0E7.15+15;2AD113.15+15;2AD13F.16+16;2AD16D.14+16;2AD199.18+15;2AD1C8.17+14;2AD1F5.10+10;2AD217.13+18;2AD244.13+D;2AD266.15+10;2AD28D.17+18;2AD2BE.17+14;2AD2EB.14+15;2AD316.16+17;2AD345.15+15;2AD371.15+C;2AD394.D+13;2AD3B6.18+16;2AD3E6.11+13;2AD40C.D+13;2AD42E.10+13;2AD453.10+13;2AD478.18+13;2AD4A5.15+18;2AD4D4.D+E;2AD4F1.17+13;2AD51D.14+16;2AD549.13+16;2AD574.15+14;2AD59F.12+16;2AD5C9.16+17;2AD5F8.11+11;2AD61C.13+11;2AD642.17+19;2AD674.14+12;2AD69C.18+18;2AD6CE.17+18;2AD6FF.15+18;2AD72E.F+13;2AD752.10+13;2AD777.E+13;2AD79A.10+13;2AD7BF.10+10;2AD7E1.15;2AD7F7.17+14;2AD824.17+17;2AD854.17+16;2AD883.17+10;2AD8AC.15+16;2AD8D9.16+14;2AD905.F+10;2AD926.10+13;2AD94B.10+13;2AD970.15+16;2AD99D.18+13;2AD9CA.10+13;2AD9EF.16+12;2ADA19.10+12;2ADA3D.10+12;2ADA61.17+16;2ADA90.17+13;2ADABC.18+10;2ADAE6.10+12;2ADB0A.10+12;2ADB2E.10+12;2ADB52.10+12;2ADB76.10+12;2ADB9A.10+12;2ADBBE.E+13;2ADBE1.18+17;2ADC12.10+12;2ADC37.17+10;2ADC60.14+C;2ADC82.F+C;2ADC9F.11+11;2ADCC3.F+D;2ADCE1.F+10;2ADD02.C+F;2ADD1F.C+D;2ADD3A.15+12;2ADD63.15+10;2ADD8A.C+C;2ADDA4.10+12;2ADDC8.18+14;2ADDF6.10+12;2ADE1A.16+15;2ADE47.13+9;2ADE65.17+12;2ADE90.14+14;2ADEBA.10+12;2ADEDE.10+12;2ADF02.13+D;2ADF24.E+18;2ADF4C.17+14;2ADF79.16+17;2ADFA8.15+14;2ADFD3.B+12;2ADFF2.18+16;2AE022.15+15;2AE04E.16+16;2AE07C.17+17;2AE0AC.17+15;2AE0DA.10+10;2AE0FC.10+10;2AE11E.10+10;2AE140.10+10;2AE162.10+10;2AE184.18+10
Place names|0|2AB8A2.5;2AB8A8.4;2AB8AD.4;2AB8B2.A;2AB8BD.9;2AB8C7.6;2AB8CE.4;2AB8D3.A;2AB8DE.5;2AB8E4.8;2AB8ED.5;2AB8F3.8;2AB8FC.5;2AB902.A;2AB90D.6;2AB914.A;2AB91F.6;2AB926.8;2AB92F.6;2AB936.A;2AB941.5;2AB947.A;2AB952.5;2AB958.5;2AB95E.A;2AB969.5;2AB96F.5;2AB975.6;2AB97C.4;2AB981.9;2AB98B.A;2AB996.6;2AB99D.5;2AB9A3.A;2AB9AE.A;2AB9B9.3;2AB9BD.A;2AB9C8.8;2AB9D1.A;2AB9DC.A;2AB9E7.A;2AB9F2.A;2AB9FD.3;2ABA01.7;2ABA09.A;2ABA14.A;2ABA1F.A;2ABA2A.A;2ABA35.A;2ABA40.8;2ABA49.9;2ABA53.8;2ABA5C.8;2ABA65.A
Planets and space travel|0|2AAA7A.7;2AAA82.7;2AAA8A.6;2AAA91.5;2AAA97.5;2AAA9D.7;2AAAA5.F;2AAAB5.13;2AAACA.13;2AAAF6.1B;2AAB12.1A;2AAB2E.1A;2AAB4A.1D;2AAB68.1D;2AAB86.15
Characters and professions|0|2A8DA0.4;2A8DA5.4;2A8DAA.4;2A8DAF.4;2A8DB4.4;2A8DB9.4;2A8DBE.4;2A8DC3.4;2A8DC8.4;2A8DCD.4;2A8DD2.4;2A8DD8.6;2A8DDF.7;2A8DE7.6;2A8DEE.8;2A8DF7.5;2A8DFD.7;2A8E05.6;2A8E0C.5
Menus and messages|0|2A9ED9.6;2A9EE1.9;2A9EED.6;2A9EF6.B;2A9F04.7;2A9F0E.7;2A9F16.3;2A9F1A.C+12;2A9F3A.14;2A9F50.7;2A9F59.7;2A9F62.7;2A9F6A.9;2A9F74.9;2A9F7F.5;2A9F87.5;2A9F8F.6;2A9F98.6;2A9FA1.6;2A9FAA.5;2A9FB2.6;2A9FBB.5;2A9FC3.5;2A9FCB.6;2A9FD4.6;2A9FDD.6;2A9FE6.6;2A9FEF.6;2A9FF6.B;2AA002.8;2AA00E.4;2AA016.4;2AA01C.4;2AA022.4;2AA028.4;2AA02E.4;2AA034.B;2AA043.B;2AA052.B;2AA061.B;2AA071.6;2AA078.B;2AA084.13;2AA098.9+F;2AA0B2.5;2AA0BA.4;2AA0C0.5;2AA0C8.4;2AA0D6.3;2AA0DB.4;2AA0E1.4;2AA0E6.15;2AA0FC.9+15;2AA11C.9;2AA127.4;2AA12E.3;2AA133.17;2AA14C.4+F;2AA162.7+D;2AA178.5;2AA17E.18+F;2AA1A8.13+15;2AA1D2.15+16;2AA200.16+15;2AA22E.16+15;2AA25C.18+15;2AA28C.C+F;2AA2AA.C+F;2AA2C8.15;2AA2DE.5;2AA304.16+D;2AA334.13+D;2AA356.E+B;2AA372.10+9;2AA38E.17+E;2AA3B5.7;2AA3BF.6;2AA3C6.6;2AA3CE.6;2AA3D8.4;2AA3E0.4;2AA3E6.8;2AA3F0.8;2AA3FA.8;2AA404.8;2AA40E.8;2AA418.8;2AA422.3;2AA427.3;2AA42E.6;2AA436.5;2AA43C.9;2AA446.8;2AA450.9;2AA45A.9;2AA464.A;2AA470.C+B;2AA48B.5;2AA493.8;2AA49E.8;2AA4A9.8;2AA4B2.3;2AA4B6.2;2AA4BB.2;2AA4C0.2;2AA4C4.2;2AA4C9.11;2AA4DD.D+13;2AA501.4;2AA508.3;2AA50C.15;2AA522.E+B;2AA53E.11;2AA550.4;2AA55E.15;2AA574.14;2AA58A.10;2AA59D.12;2AA5B2.11;2AA5C6.12;2AA5DB.11;2AA5EF.12;2AA604.12;2AA618.7;2AA620.17;2AA639.F+11;2AA65C.C;2AA66A.5;2AA672.6;2AA67B.6;2AA684.6;2AA68C.8;2AA696.3;2AA69B.3;2AA6A0.6;2AA6A8.A;2AA6B4.A;2AA6C0.C;2AA6CE.6;2AA6DE.2;2AA6E2.5;2AA6EC.6;2AA6F4.5;2AA6FA.7;2AA702.6;2AA70A.13;2AA71E.5;2AA724.3;2AA728.11;2AA73A.D;2AA748.11;2AA75A.E+B;2AA776.D+11;2AA796.15+E;2AA7BC.E;2AA7CC.D;2AA7DA.B;2AA7E6.9;2AA7F0.A;2AA7FC.15+B;2AA81F.4;2AA826.4;2AA82D.4;2AA834.4;2AA83A.4;2AA840.4;2AA846.3;2AA84A.B+E;2AA866.E;2AA876.10+9;2AA892.3;2AA896.10+13;2AA8BC.14;2AA8D2.B;2AA8E5.F;2AA8F9.11;2AA90D.A;2AA919.B;2AA927.9;2AA932.B;2AA93F.A;2AA94B.A;2AA957.13;2AA96D.14;2AA983.10;2AA995.15;2AA9AD.17;2AA9C7.15;2AA9DF.16;2AA9F6.4;2AA9FC.5;2AAA02.3;2AAA06.B+12;2AAA26.A+12;2AAA46.18;2AAA60.8+10
Inns and shops|1|2AC0DE.8+10;2AC0F8.8+17;2AC11A.10+1C;2AC148.1C;2AC166.14+12;2AC18E.F+12;2AC1B2.8+10;2AC1CC.8+16;2AC1EC.12+1C;2AC21C.19;2AC236.A+B;2AC24E.13+B;2AC26E.10+4;2AC284.7;2AC28E.16;2AC2A6.14+E;2AC2CA.10+13;2AC2F0.1A;2AC30C.F+1E;2AC33C.13+1C;2AC36E.1C;2AC38C.1C;2AC3AE.8+F;2AC3C8.14+F;2AC3EE.13+17;2AC41A.D+1E;2AC448.13+12;2AC470.14+15;2AC49C.F;2AC4AC.7;2AC4B4.11;2AC4C8.D;2AC4D6.A+F;2AC4F2.16+F;2AC51A.1C;2AC538.12+19;2AC566.12+1C;2AC596.1C;2AC5B4.17;2AC5D0.8+A;2AC5E4.7+20;2AC60E.E+F;2AC62E.9+1D;2AC656.A+12;2AC674.11+19;2AC6A0.7;2AC6A8.5;2AC6AE.4;2AC6B4.8+1C;2AC6DA.18;2AC6F3.20+17;2AC72E.D;2AC73C.1A+1F;2AC778.E+13;2AC79C.13+15;2AC7C6.12+14;2AC7EE.17+1C;2AC824.20;2AC846.1D;2AC864.F;2AC874.D+13;2AC896.E+13;2AC8BA.12+15;2AC8E4.15+14;2AC910.1A+17
Buy, sell, equip|0|2AE1AF.4;2AE1B5.5;2AE1BC.5
Things looked at (short)|1|1FE662.1B+5;1FE686.1A+D;1FE6B1.17+1A;1FE6E6.1C+5;1FE70B.13;1FE721.19;1FE73D.14+13;1FE768.18;1FE783.8+14;1FE7A3.1C;1FE7C2.B+16;1FE7E8.1E+18;1FE820.19;1FE83C.D;1FE84C.1F;1FE86E.1F;1FE890.F;1FE8A2.F;1FE8B4.8+1F;1FE8DD.1E;1FE8FE.1D;1FE91E.11;1FE932.1A+8;1FE958.17+9;1FE97C.1A+1B;1FE9B6.1C+20;1FE9F4.1C;1FEA14.4+1C;1FEA36.13;1FEA4C.A+11;1FEA69.7;1FEA74.15;1FEA8A.1C+1F;1FEACA.18+1A;1FEAFE.A;1FEB0B.1C+19;1FEB43.9;1FEB50.B;1FEB5F.A;1FEB6D.9;1FEB7A.7;1FEB85.B;1FEB93.9;1FEB9D.9;1FEBA7.B;1FEBB3.6;1FEBBB.8;1FEBC4.9;1FEBD0.B;1FEBDF.A;1FEBED.9;1FEBFA.8;1FEC06.9;1FEC13.9;1FEC1D.B;1FEC2A.9;1FEC34.B;1FEC40.9;1FEC4A.6;1FEC51.B;1FEC5D.9;1FEC67.9;1FEC71.8;1FEC7B.B;1FEC8A.9;1FEC97.B;1FECA6.A;1FECB4.9;1FECC0.9;1FECCA.8;1FECD3.9;1FECDD.6;1FECE5.A;1FECF0.A;1FECFD.A;1FED08.9;1FED14.A;1FED1F.A;1FED2B.8;1FED34.B;1FED40.7;1FED48.A;1FED53.A;1FED5E.8;1FED67.7;1FED6F.9;1FED79.8;1FED82.A;1FED8D.8;1FED96.8;1FED9F.8;1FEDA8.A;1FEDB3.8;1FEDBC.A;1FEDC7.5;1FEDCD.8;1FEDD6.B;1FEDE2.9;1FEDEC.6;1FEDF3.A;1FEDFE.8;1FEE07.8;1FEE11.8;1FEE1A.9;1FEE26.13;1FEE3A.6;1FEE41.F;1FEE51.12;1FEE64.E;1FEE73.15;1FEE89.16;1FEEA0.11;1FEEB2.B;1FEEBE.E;1FEECD.15;1FEEE3.14;1FEEF8.10;1FEF09.F;1FEF19.A;1FEF24.D;1FEF32.B;1FEF3E.11;1FEF50.10;1FEF61.D;1FEF6F.14;1FEF84.14;1FEF99.D;1FEFA7.D;1FEFB5.C;1FEFC2.E;1FEFD1.E;1FEFE0.E;1FEFEF.E;1FEFFE.E
Hunters' Guild requests|0|71DCC.F;71DDC.10;71DEE.F;71DFE.F;71E0E.D;71E1C.9;71E26.E;71E36.E;71E46.F
Vehicles|0|6C024.F
The opening's words|1|2AAB9C.1A;2AABB7.1E;2AABD6.15;2AABEC.1E;2AAC0B.12;2AAC1E.16;2AAC35.17;2AAC4D.19;2AAC68.16;2AAC7F.15;2AAC96.1D;2AACB4.10;2AACC5.1F;2AACE5.18;2AACFE.17;2AAD16.9;2AAD21.1A;2AAD3C.7;2AAD44.1A;2AAD5F.1B;2AAD7B.A;2AAD86.1F;2AADA6.18;2AADBF.18;2AADD8.1E;2AADF7.9;2AAE02.11;2AAE14.19;2AAE2E.1C;2AAE4B.1C;2AAE68.17;2AAE81.18;2AAE9B.1D;2AAEB9.20;2AAEDA.7;2AAEE2.1B;2AAEFE.1B;2AAF1A.16;2AAF31.D;2AAF40.17;2AAF58.1C;2AAF75.1F;2AAF95.1A
"""

COLUMNS = ['id', 'where', 'speaker', 'english', 'translation']
PAGE = '{page}'
CUT = '{cut}'                      # $F7: the box closes or the scene does something, then the line goes on
BREAK = re.compile(r'\{page\}|\{cut\}')
LINE_UNITS = 32                    # the box's line: 32 letters; a full-width character counts as two
PAGE_LINES = 2


class Problem(Exception):
    pass


# ---------------------------------------------------------------------------- the ROM
def read_rom(path):
    data = open(path, 'rb').read()
    if path.lower().endswith('.zip') or data[:2] == b'PK':
        import zipfile
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            names = [n for n in z.namelist() if n.lower().endswith(('.bin', '.md', '.gen', '.smd'))]
            if not names:
                raise Problem('no ROM inside %s' % path)
            data = z.read(names[0])
    if len(data) % 16384 == 512:                           # a .smd: a header, then blocks with the
        body, out = data[512:], bytearray(len(data) - 512)  # odd bytes first, the even ones after
        for b in range(0, len(body), 16384):
            blk = body[b:b + 16384]
            out[b + 1:b + 16384:2] = blk[:8192]
            out[b:b + 16384:2] = blk[8192:]
        data = bytes(out)
    if hashlib.sha1(data).hexdigest() != ROM_SHA1:
        raise Problem('%s is not the Phantasy Star IV (USA) ROM this tool reads (as PSIV Recomp does).' % path)
    return data


def kosinski(data, at):
    """the bytes a Kosinski stream unpacks to"""
    out = bytearray()
    pos = at + 2
    desc, bits = data[at] | data[at + 1] << 8, 16

    def bit():
        nonlocal desc, bits, pos
        b = desc & 1
        desc >>= 1
        bits -= 1
        if not bits:
            desc, bits = data[pos] | data[pos + 1] << 8, 16
            pos += 2
        return b

    while True:
        if bit():
            out.append(data[pos])
            pos += 1
            continue
        if bit():
            lo, hi = data[pos], data[pos + 1]
            pos += 2
            offset = (hi & 0xF8) << 5 | lo
            offset -= 0x2000
            count = hi & 7
            if count:
                count += 2
            else:
                count = data[pos]
                pos += 1
                if count == 0:
                    break
                if count == 1:
                    continue
                count += 1
        else:
            count = (bit() << 1 | bit()) + 2
            offset = data[pos] - 0x100
            pos += 1
        for _ in range(count):
            out.append(out[offset])                        # (offset is negative: from the end)
    return bytes(out)


# ---------------------------------------------------------------------------- a line, as text
def tag(code, args=()):
    return '{%02X%s}' % (code, ':' + ','.join('%02X' % a for a in args) if args else '')


def split_lines(bank_no, raw):
    """a bank's bytes -> its lines, each a list of tokens: ('t', text) or ('c', code, args)"""
    lines, cur, text, i = [], [], [], 0

    def flush():
        if text:
            cur.append(('t', ''.join(text)))
            text.clear()

    while i < len(raw):
        b = raw[i]
        i += 1
        if b < 0xF0:
            text.append(CHARS.get(b) or '{%02X}' % b)
            continue
        flush()
        if b == 0xFF:
            lines.append(cur)
            cur = []
            continue
        n = 0
        if b == 0xF2:
            n = 1 + ACTION_BYTES.get(raw[i] if i < len(raw) else 0, 0)
        elif b == 0xF4:
            n = 2 if bank_no in TWO_BYTE_PORTRAIT else 1
        elif b in (0xF5, 0xF6, 0xFA):                      # (the event's number is a word)
            n = 2
        elif b == 0xF9:
            n = 1
        cur.append(('c', b, tuple(raw[i:i + n])))
        i += n
    flush()
    if cur:                                                # (the padding after the last line)
        if any(t[0] == 't' and t[1].strip() for t in cur):
            lines.append(cur)
    return lines


def line_text(tokens):
    """the workbook's cell: letters, a new line for $FC, {page} ($FD) and {cut} ($F7) each on a line of
    its own, and a {tag} for every other code"""
    out = []
    for t in tokens:
        if t[0] == 't':
            out.append(t[1])
        elif t[1] == 0xFC:
            out.append('\n')
        elif t[1] == 0xFD:
            out.append('\n' + PAGE + '\n')
        elif t[1] == 0xF7:
            out.append('\n' + CUT + '\n')
        else:
            out.append(tag(t[1], t[2]))
    return re.sub(r'\n*(\{page\}|\{cut\})\n*', r'\n\1\n', ''.join(out)).strip('\n')


def speakers(tokens):
    seen = []
    for t in tokens:
        if t[0] == 'c' and t[1] == 0xF4 and t[2] and t[2][0]:
            name = PORTRAITS.get(t[2][0], 'portrait %02X' % t[2][0])
            if name not in seen:
                seen.append(name)
    return ', '.join(seen)


def has_words(tokens):
    return any(t[0] == 't' and re.search(r'[A-Za-z0-9]', re.sub(r'\{..\}', '', t[1])) for t in tokens)


def short_lines(rom):
    """the names and messages: {id: (heading, '', text)}, id = T and the entry's address"""
    out = {}
    for row in SHORT_TABLES.strip().split('\n'):
        name, cs, entries = row.split('|')
        chars = CHARS if cs == '1' else WIN_CHARS
        for e in entries.split(';'):
            at, lens = e.split('.')
            pos, parts = int(at, 16), []
            for n in lens.split('+'):
                n = int(n, 16)
                parts.append(''.join(chars.get(b, '?') for b in rom[pos:pos + n]))
                pos += n + 1
            if re.search(r'[A-Za-z0-9]', ''.join(parts)):  # (not the windows' bare colons and rules)
                out['T%06X' % int(at, 16)] = (name, '', '\n'.join(parts))
    return out


def game_lines(rom):
    """every line of dialogue with words in it: {id: (where, speaker, text)}; and how many there are"""
    out, total = {}, 0
    for n, at in enumerate(BANKS, 1):
        for k, tokens in enumerate(split_lines(n, kosinski(rom, at))):
            total += 1
            if has_words(tokens):
                out['D%02d.%03X' % (n, k)] = (WHERE[n - 1], speakers(tokens), line_text(tokens))
    out.update(short_lines(rom))
    return out, total


# ---------------------------------------------------------------------------- the workbook
def read_workbook(path):
    with open(path, encoding='utf-8-sig', newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows or any(c not in rows[0] for c in ('id', 'translation')):
        raise Problem('%s is not a workbook of this tool (no id / translation columns).' % path)
    for r in rows:
        for c in COLUMNS:
            r[c] = (r.get(c) or '').replace('\r\n', '\n').replace('\r', '\n')
    return rows


TAG = re.compile(r'\{[^{}]*\}')


def tags_of(text):
    """the codes a text carries, in order, without the page ends (those are counted apart)"""
    return [t for t in TAG.findall(text) if t != PAGE]


def width(s):
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in TAG.sub('', s))


def check_rows(rows, originals):
    """-> (problems, warnings, translated): each a list of (id, words)"""
    problems, warnings, done, seen = [], [], 0, set()
    for r in rows:
        i, tr = r['id'].strip(), r['translation']
        if not i:
            continue
        if i in seen:
            problems.append((i, 'this line is in the workbook twice'))
        seen.add(i)
        en = originals[i][2] if originals and i in originals else r['english']
        if originals is not None and i not in originals:
            problems.append((i, 'there is no such line in the game'))
            continue
        if not tr.strip():
            continue
        done += 1
        stray = TAG.sub('', tr)
        if '{' in stray or '}' in stray:
            problems.append((i, 'a { or } that is not part of a code'))
        if i.startswith('T'):                              # a name or a message: no codes, no boxes
            if TAG.search(tr):
                problems.append((i, 'a name or a message takes no codes'))
            a, b = len(en.split('\n')), len(tr.strip('\n').split('\n'))
            if a != b:
                warnings.append((i, '%d lines where the game\'s has %d' % (b, a)))
            continue
        if tags_of(tr) != tags_of(en):
            problems.append((i, 'the codes differ from the original: it has %s, the translation %s'
                             % (' '.join(tags_of(en)) or 'none', ' '.join(tags_of(tr)) or 'none')))
        if tr.count(PAGE) != en.count(PAGE):
            warnings.append((i, '%d pages where the original has %d' % (tr.count(PAGE) + 1, en.count(PAGE) + 1)))
        for page in BREAK.split(tr):
            lines = [x for x in page.strip('\n').split('\n')]
            if len(lines) > PAGE_LINES:
                warnings.append((i, 'a page of %d lines (the box shows %d)' % (len(lines), PAGE_LINES)))
            for x in lines:
                if width(x) > LINE_UNITS:
                    warnings.append((i, 'a line %d wide (the box is %d: a full-width character counts as two): %s'
                                     % (width(x), LINE_UNITS, x.strip()[:40])))
    return problems, warnings, done


# ---------------------------------------------------------------------------- the commands
def cmd_extract(a):
    lines, total = game_lines(read_rom(a.rom))
    kept = {}
    try:                                                   # a workbook already there: its work is kept
        for r in read_workbook(a.output):
            if r['translation'].strip():
                kept[r['id'].strip()] = r['translation']
    except (OSError, Problem):
        pass
    with open(a.output, 'w', encoding='utf-8-sig', newline='') as f:
        w = csv.writer(f, lineterminator='\r\n')
        w.writerow(COLUMNS)
        for i, (where, who, text) in lines.items():
            w.writerow([i, where, who, text, kept.get(i, '')])
    short = sum(1 for i in lines if i.startswith('T'))
    print('%s: %d lines of dialogue (of the game\'s %d; the rest are codes alone) and %d names and messages%s'
          % (a.output, len(lines) - short, total, short, '; %d translations kept' % len(kept) if kept else ''))
    print('It holds the game\'s own words: keep it to yourself. "pack" writes the file to share.')


def report(rows, originals):
    problems, warnings, done = check_rows(rows, originals)
    total = len(originals) if originals else len([r for r in rows if r['id'].strip()])
    for i, words in problems:
        print('PROBLEM  %s: %s' % (i, words))
    for i, words in warnings:
        print('warning  %s: %s' % (i, words))
    print('%d of %d lines translated (%.1f%%); %d problems, %d warnings'
          % (done, total, 100.0 * done / total if total else 0, len(problems), len(warnings)))
    return problems


def cmd_check(a):
    rows = read_workbook(a.workbook)
    originals = game_lines(read_rom(a.rom))[0] if a.rom else None
    if originals is None:
        print('(no --rom: checked against the workbook\'s own english column)')
    return 1 if report(rows, originals) else 0


def cmd_pack(a):
    rows = read_workbook(a.workbook)
    originals = game_lines(read_rom(a.rom))[0] if a.rom else None
    if report(rows, originals):
        raise Problem('not packed: the problems above come first.')
    out = a.output or 'psiv-%s.txt' % re.sub(r'[^A-Za-z0-9_-]+', '-', a.language).strip('-').lower()
    n = 0
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('# PSIV Recomp translation pack\nformat = 1\nlanguage = %s\n' % a.language)
        for r in rows:
            if r['id'].strip() and r['translation'].strip():
                f.write('\n[%s]\n%s\n' % (r['id'].strip(), r['translation'].strip('\n')))
                n += 1
    print('%s: %d lines, your translation alone' % (out, n))


def main():
    p = argparse.ArgumentParser(description='PSIV Recomp: the translators\' tool')
    sub = p.add_subparsers(dest='cmd', required=True)
    e = sub.add_parser('extract', help='your ROM\'s dialogue into a workbook')
    e.add_argument('rom')
    e.add_argument('-o', '--output', default='psiv-dialogue.csv')
    c = sub.add_parser('check', help='look a workbook\'s translation over')
    c.add_argument('workbook')
    c.add_argument('--rom', help='check against the game itself, not the english column')
    k = sub.add_parser('pack', help='the file to share: your translation alone')
    k.add_argument('workbook')
    k.add_argument('--language', required=True, help='its name as players will see it')
    k.add_argument('-o', '--output')
    k.add_argument('--rom')
    a = p.parse_args()
    for stream in (sys.stdout, sys.stderr):                # a console that lacks a character shows "?"
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(errors='replace')
    try:
        return {'extract': cmd_extract, 'check': cmd_check, 'pack': cmd_pack}[a.cmd](a) or 0
    except Problem as ex:
        print(ex, file=sys.stderr)
        return 1
    except OSError as ex:
        print('%s: %s' % (ex.filename, ex.strerror), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
