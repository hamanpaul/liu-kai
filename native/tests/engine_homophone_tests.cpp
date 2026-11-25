#include "../dictionary.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#define CHECK(x) do { if(!(x)){std::cerr << #x << " line " << __LINE__ << '\n';return 1;} } while(0)
int main(int argc,char** argv){
 using namespace boshiamy;
 std::istringstream primary_reading_source(
     "A\treading secondary\t3\n"
     "B\tsecondary reading\t2\n"
     "C\treading\t1\n");
 const auto primary_readings=Readings::parse(primary_reading_source);
 CHECK(primary_readings.matches_primary_reading("reading")==std::vector<std::string>({"A","C"}));
 CHECK(primary_readings.primary_matches("A")==std::vector<std::string>({"A","C"}));
 CHECK(primary_readings.matches_primary_reading("missing").empty());
 CHECK(zhuyin::reading("a87")==std::optional<std::string>("˙ㄇㄚ"));

 Engine reading_engine;
 std::ostringstream phonetic_rows;
 for(int i=0;i<12;++i){
  const char text=static_cast<char>('A'+i);
  reading_engine.add(std::string(1,static_cast<char>('a'+i)),{std::string(1,text)});
  phonetic_rows << text << '\t' << "ㄇㄚ\t" << i+1 << '\n';
 }
 reading_engine.add("zz",{"A"});
 reading_engine.add("m",{"M"}); reading_engine.add("p",{"P"}); reading_engine.add("q",{"Q"});
 reading_engine.add("r",{"R"}); reading_engine.add("t",{"T"},Mode::Simplified);
 reading_engine.add("u",{"U"}); reading_engine.add("v",{"V"}); reading_engine.add("w",{"W"});
 phonetic_rows << "M\tㄧ ㄇㄚ\t0\nN\tㄇㄚ\t0\nP\tㄇㄚˊ\t13\nQ\tㄆㄚ\t14\nR\tㄇㄚˇ\t15\n"
                  "T\tㄇㄚ\t16\nU\tㄇㄚˋ\t17\nV\t˙ㄇㄚ\t18\nW\tㄇㄧㄚˋ\t19\n";
 std::istringstream phonetic_readings(phonetic_rows.str());
 reading_engine.set_readings(Readings::parse(phonetic_readings));
 auto open_phonetic=[&](){
  reading_engine.key('\''); reading_engine.key(';'); reading_engine.key('a'); reading_engine.key('8');
  return reading_engine.key(Key::Space);
 };
 reading_engine.key('\''); reading_engine.key(';'); reading_engine.key('d'); reading_engine.key('k');
 auto no_result_before_engine_reconnect=reading_engine.key(Key::Space);
 CHECK(no_result_before_engine_reconnect.query==QueryStage::PhoneticInput);
 auto no_result_enter_before_engine_reconnect=reading_engine.key(Key::Enter);
 CHECK(no_result_enter_before_engine_reconnect.commit.empty());
 reading_engine.key(Key::Escape);
 reading_engine.set_reverse_mode(ReverseMode::All);
 auto reading_list=open_phonetic();
 CHECK(reading_list.query==QueryStage::Homophones&&reading_list.commit.empty());
 CHECK(reading_engine.is_phonetic_input()&&reading_engine.phonetic_confirmation_key()==' ');
 CHECK(reading_list.candidates.size()==10&&reading_list.candidates.front().text=="A"&&reading_list.candidates.back().text=="J");
 CHECK(reading_list.candidates.front().reverse_code=="a / zz");
 CHECK(candidate_display_text(reading_list.candidates.front())=="A A / ZZ");
 CHECK(reading_list.composition=="注"+zhuyin::display("a8"));
 auto reading_page=reading_engine.key(Key::Space);
 CHECK(reading_page.query==QueryStage::Homophones&&reading_page.page==1);
 CHECK(reading_engine.phonetic_confirmation_key()==0);
 CHECK(reading_page.candidates.size()==2&&reading_page.candidates.front().text=="K");
 auto wrapped_page=reading_engine.key(Key::Space);
 CHECK(wrapped_page.page==0&&wrapped_page.candidates.front().text=="A");
 auto reading_down=reading_engine.key(Key::Down);
 CHECK(reading_down.query==QueryStage::Homophones&&reading_down.candidate_selection==1&&reading_down.commit.empty());
 auto reading_enter=reading_engine.key(Key::Enter);
 CHECK(reading_enter.commit=="B"&&reading_enter.query==QueryStage::None&&reading_enter.composition.empty());
 CHECK(!reading_engine.is_phonetic_input()&&reading_engine.phonetic_confirmation_key()==0);
 auto normal_after_reading=reading_engine.key('s');
 CHECK(normal_after_reading.query==QueryStage::None&&normal_after_reading.composition=="s");
 reading_engine.key(Key::Escape);
 reading_list=open_phonetic();
 CHECK(reading_list.candidates.front().text=="A");
 auto reading_numeric=reading_engine.key('0');
 CHECK(reading_numeric.commit=="A"&&reading_numeric.query==QueryStage::None);
 reading_list=open_phonetic();
 reading_engine.set_mode(Mode::Simplified);
 CHECK(reading_engine.snapshot().query==QueryStage::None&&reading_engine.snapshot().composition.empty());
 auto simplified_reading=open_phonetic();
 CHECK(simplified_reading.query==QueryStage::Homophones&&simplified_reading.candidates.size()==1);
 CHECK(simplified_reading.candidates.front().text=="T"&&simplified_reading.candidates.front().reverse_code=="t");
 CHECK(reading_engine.key('0').commit=="T");
 reading_engine.set_mode(Mode::Traditional);

 reading_list=open_phonetic();
 auto reading_backspace=reading_engine.key(Key::Backspace);
 CHECK(reading_backspace.query==QueryStage::PhoneticInput);
 CHECK(reading_backspace.composition=="注"+zhuyin::display("a8"));
 reading_engine.key('q');
 auto corrected_reading=reading_engine.key(Key::Space);
 CHECK(corrected_reading.query==QueryStage::Homophones&&corrected_reading.candidates.size()==1);
 CHECK(corrected_reading.candidates.front().text=="Q");
 CHECK(reading_engine.key('0').commit=="Q");

 reading_engine.key('\''); reading_engine.key(';'); reading_engine.key('d'); reading_engine.key('k');
 auto missing_reading=reading_engine.key(Key::Space);
 CHECK(missing_reading.query==QueryStage::PhoneticInput&&missing_reading.candidates.empty());
 CHECK(missing_reading.commit.empty());
 auto missing_enter=reading_engine.key(Key::Enter);
 CHECK(missing_enter.commit.empty()&&missing_enter.query==QueryStage::PhoneticInput);
 auto missing_backspace=reading_engine.key(Key::Backspace);
 CHECK(missing_backspace.query==QueryStage::PhoneticInput);
 CHECK(missing_backspace.composition=="注"+zhuyin::display("d"));
 auto missing_escape=reading_engine.key(Key::Escape);
 CHECK(missing_escape.commit.empty()&&missing_escape.query==QueryStage::None&&missing_escape.composition.empty());

 reading_engine.key('\''); reading_engine.key(';'); reading_engine.key('a'); reading_engine.key('8');
 auto second_tone=reading_engine.key('6');
 CHECK(second_tone.query==QueryStage::Homophones&&second_tone.commit.empty());
 CHECK(reading_engine.phonetic_confirmation_key()=='6');
 CHECK(second_tone.candidates.size()==1&&second_tone.candidates.front().text=="P");
 auto tone_backspace=reading_engine.key(Key::Backspace);
 CHECK(tone_backspace.query==QueryStage::PhoneticInput);
 CHECK(reading_engine.phonetic_confirmation_key()==0);
 CHECK(tone_backspace.composition=="注"+zhuyin::display("a86"));
 auto remove_second_tone=reading_engine.key(Key::Backspace);
 CHECK(remove_second_tone.query==QueryStage::PhoneticInput);
 CHECK(remove_second_tone.composition=="注"+zhuyin::display("a8"));
 auto third_tone=reading_engine.key('3');
 CHECK(third_tone.query==QueryStage::Homophones&&third_tone.candidates.size()==1);
 CHECK(reading_engine.phonetic_confirmation_key()=='3');
 CHECK(third_tone.candidates.front().text=="R"&&third_tone.commit.empty());
 auto third_backspace=reading_engine.key(Key::Backspace);
 CHECK(third_backspace.query==QueryStage::PhoneticInput);
 CHECK(reading_engine.phonetic_confirmation_key()==0);
 CHECK(third_backspace.composition=="注"+zhuyin::display("a83"));
 auto third_reconfirm=reading_engine.key(Key::Space);
 CHECK(third_reconfirm.query==QueryStage::Homophones&&third_reconfirm.commit.empty());
 CHECK(reading_engine.phonetic_confirmation_key()==' ');
 auto re_confirm_space_page=reading_engine.key(Key::Space);
 CHECK(re_confirm_space_page.query==QueryStage::Homophones&&reading_engine.phonetic_confirmation_key()==0);
 CHECK(reading_engine.key(Key::Backspace).query==QueryStage::PhoneticInput);
 reading_engine.key(Key::Backspace);
 auto fourth_tone=reading_engine.key('4');
 CHECK(fourth_tone.query==QueryStage::Homophones&&fourth_tone.candidates.size()==1);
 CHECK(fourth_tone.candidates.front().text=="U"&&fourth_tone.commit.empty());
 CHECK(reading_engine.key('0').commit=="U");

 reading_engine.key('\''); reading_engine.key(';'); reading_engine.key('a'); reading_engine.key('8');
 auto neutral_key=reading_engine.key('7');
 CHECK(neutral_key.query==QueryStage::PhoneticInput&&neutral_key.commit.empty());
 CHECK(reading_engine.phonetic_confirmation_key()==0);
 auto neutral_confirm=reading_engine.key(Key::Space);
 CHECK(neutral_confirm.query==QueryStage::Homophones&&neutral_confirm.commit.empty());
 CHECK(reading_engine.phonetic_confirmation_key()==' ');
 CHECK(neutral_confirm.candidates.size()==1&&neutral_confirm.candidates.front().text=="V");
 CHECK(reading_engine.key('0').commit=="V");

 reading_engine.key('\''); reading_engine.key(';'); reading_engine.key('q'); reading_engine.key('u');
 reading_engine.key('8'); auto full_no_result=reading_engine.key('4');
 CHECK(full_no_result.query==QueryStage::PhoneticInput&&full_no_result.candidates.empty());
 CHECK(full_no_result.composition=="注"+zhuyin::display("qu84"));
 auto full_corrected=reading_engine.key('a');
 CHECK(full_corrected.query==QueryStage::Homophones&&full_corrected.candidates.size()==1);
 CHECK(full_corrected.candidates.front().text=="W"&&full_corrected.commit.empty());
 CHECK(reading_engine.key('0').commit=="W");

 Engine extension_engine;
 extension_engine.add("a",{"㐀"});
 std::istringstream extension_readings("㐀\tㄇㄚ\n");
 extension_engine.set_readings(Readings::parse(extension_readings));
 extension_engine.set_show_extensions(false);
 extension_engine.key('\''); extension_engine.key(';'); extension_engine.key('a'); extension_engine.key('8');
 auto hidden_from_start=extension_engine.key(Key::Space);
 CHECK(hidden_from_start.query==QueryStage::PhoneticInput&&hidden_from_start.candidates.empty());
 extension_engine.key(Key::Escape);
 extension_engine.set_show_extensions(true);
 extension_engine.key('\''); extension_engine.key(';'); extension_engine.key('a'); extension_engine.key('8');
 auto extension_list=extension_engine.key(Key::Space);
 CHECK(extension_list.query==QueryStage::Homophones&&extension_list.candidates.size()==1);
 extension_engine.set_show_extensions(false);
 CHECK(extension_engine.snapshot().query==QueryStage::PhoneticInput);
 CHECK(extension_engine.snapshot().candidates.empty());
 CHECK(extension_engine.snapshot().composition=="注"+zhuyin::display("a8"));
 CHECK(extension_engine.key(Key::Enter).commit.empty());
 extension_engine.key(Key::Escape);

 Engine e; e.add("s",{"Z"});
 std::ostringstream rows; rows << "mode\tseed\tordinal\tcandidate_text\n";
 for(int i=0;i<12;++i){std::string text(1,static_cast<char>('A'+i)); e.add(std::string(1,static_cast<char>('a'+i)),{text});rows << "traditional\tZ\t" << i+1 << '\t' << text << '\n';}
 std::istringstream readings("Z\treading\n");e.set_readings(Readings::parse(readings));
 std::istringstream index(rows.str());e.set_observed_primary_homophones(ObservedPrimaryHomophoneIndex::parse(index));
 e.key('\'');e.key('s');auto seed=e.snapshot();CHECK(seed.query==QueryStage::HomophoneSeed);CHECK(seed.candidates[0].text=="Z");
 auto list=e.key(Key::Space);CHECK(list.query==QueryStage::Homophones);CHECK(list.commit.empty());CHECK(list.candidates.size()==10);CHECK(list.candidates[0].text=="A");CHECK(candidate_display_text(list.candidates[0])=="A A");
 auto second=e.key(Key::Space);CHECK(second.query==QueryStage::Homophones);CHECK(second.page==1);CHECK(second.candidates[0].text=="K");
 auto picked=e.key('0');CHECK(picked.commit=="K");CHECK(picked.hint.empty());CHECK(picked.query==QueryStage::None);CHECK(picked.composition.empty());
 e.key('\'');e.key('s');e.key(Key::Space);e.key(Key::Space);auto wrap=e.key(Key::Space);CHECK(wrap.page==0);CHECK(wrap.candidates[0].text=="A");
 auto picked2=e.key('1');CHECK(picked2.commit=="B");CHECK(picked2.query==QueryStage::None);
 e.key('\'');e.key('s');e.key(Key::Space);auto down=e.key(Key::Down);CHECK(down.candidate_selection==1&&down.commit.empty());
 auto up=e.key(Key::Up);CHECK(up.candidate_selection==0&&up.commit.empty());
 auto last=e.key(Key::Up);CHECK(last.page==1&&last.candidate_selection==11);CHECK(e.key(Key::Enter).commit=="L");
 e.key('\'');e.key('s');e.key(Key::Space);CHECK(e.key(Key::Enter).commit=="A");
 e.key('\'');e.key('s');e.key(Key::Space);for(int i=0;i<10;++i)e.key(Key::Down);CHECK(e.snapshot().page==1&&e.snapshot().candidate_selection==10);CHECK(e.key(Key::Enter).commit=="K");
 e.key('\'');e.key('s');e.key(Key::Space);auto cancel=e.key(Key::Escape);CHECK(cancel.commit.empty());CHECK(cancel.query==QueryStage::None);
 e.key('s');CHECK(e.key(Key::Space).commit=="Z");
 CHECK(argc==4); std::ifstream dictionary(argv[1],std::ios::binary), actualReadings(argv[2],std::ios::binary), phonetic(argv[3],std::ios::binary);
 CHECK(dictionary && actualReadings && phonetic); auto real=load_dictionary(dictionary).make_engine();
 real.set_readings(Readings::parse(actualReadings));real.set_phonetic_candidates(PhoneticCandidateIndex::parse(phonetic));real.set_observed_primary_homophones(ObservedPrimaryHomophoneIndex::built_in());
 real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto actual=real.key(Key::Space);
 CHECK(actual.query==QueryStage::Homophones);CHECK(!actual.candidates.empty());CHECK(actual.candidates[0].text=="\xe8\x9d\xa6");
 CHECK(candidate_display_text(actual.candidates[0])=="蝦 CFA");auto shrimp=real.key('0');CHECK(shrimp.commit=="\xe8\x9d\xa6");CHECK(shrimp.hint.empty());
  real.key('\'');real.key(';');for(char c:std::string("vu8"))real.key(c);
  auto directReading=real.key(Key::Space);
  CHECK(directReading.query==QueryStage::Homophones&&directReading.commit.empty());
  CHECK(directReading.layout_candidates&&std::any_of(directReading.layout_candidates->begin(),directReading.layout_candidates->end(),
      [](const Candidate& candidate){return candidate.text=="\xe8\x9d\xa6"&&!candidate.reverse_code.empty();}));
  real.key(Key::Escape);
  real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto originalH3Again=real.key(Key::Space);
  CHECK(originalH3Again.query==QueryStage::Homophones&&!originalH3Again.candidates.empty());
  CHECK(originalH3Again.candidates[0].text=="\xe8\x9d\xa6");
  real.key(Key::Escape);
 std::istringstream recommended("section\ttext\tcode\nTRADITIONAL\t蝦\tcofu\n");real.set_recommendations(RecommendedCodes::parse(recommended));
 real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto recommendedList=real.key(Key::Space);CHECK(candidate_display_text(recommendedList.candidates[0])=="蝦 COFU");
 auto recommendedShrimp=real.key('0');CHECK(recommendedShrimp.commit=="蝦");CHECK(recommendedShrimp.hint.empty());
 real.set_reverse_mode(ReverseMode::All);real.key('\'');for(char c:std::string("mnfo"))real.key(c);auto allList=real.key(Key::Space);CHECK(candidate_display_text(allList.candidates[0])=="蝦 CFA / COX / COFU");
 auto allShrimp=real.key('0');CHECK(allShrimp.commit=="蝦");CHECK(allShrimp.hint.empty());
 std::cout << "homophone_query_tests_passed\n";return 0;
}
