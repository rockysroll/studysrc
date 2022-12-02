#include<stdio.h>
#include <vector>
#include <map>
#include <iostream>
#include <ctime>
int main(){
	std::map<int,std::string> plus;
	int first = 0;
	int second = 0;
	int index = 0;
	char string[256];
	std::srand(std::time(0));
	for(first = 0;first <11; first++){
		for(second = 0;second <11; second++){
			sprintf(string,"%d - %d = ",first,second);
			if(-1 < (first-second) && (first-second) < 11 ){
				if((0 == (first-second)) || (0 == second) || (0 == first)){
					plus.insert(std::pair<int,std::string>(index, std::string(string)));
					index++;
				}
			}
		}
	}

	for(first = 0;first <11; first++){
		for(second = 0;second <11; second++){
			sprintf(string,"%d + %d = ",first,second);
			if(11 > (first+second) ){
				if((0 == (first+second)) || (0 == second) || (0 == first)){
					plus.insert(std::pair<int,std::string>(index, std::string(string)));
					index++;
					break;
				}
			}
		}
	}
	std::cout << "the index = " << index << std::endl;

	//for (std::map<int,std::string>::iterator it = plus.begin();it != plus.end(); ++it)
	//	std::cout << (*it).second << std::endl;

	for(int i = 1;i < 101; i++){
		std::cout << plus[rand()%(32-0)]  << ",";
		if(0 == (i%5)) std::cout << std::endl;
	}

	return 0;
}
