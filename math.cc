#include<stdio.h>
#include <vector>
#include <map>
#include <iostream>
int main(){
	std::map<int,std::string> plus;
	int first = 0;
	int second = 0;
	int index = 0;
	char string[256];
	for(first = 1;first <11; first++){
		for(second = 1;second <11; second++){
			sprintf(string,"%d + %d = ",first,second);
			if(11 > (first+second) ){
				//printf("%s\n",string);
				plus.insert(std::pair<int,std::string>(index, std::string(string)));
				index++;
			}
		}
	}
	//std::cout << "the index = " << index << std::endl;

	//for (std::map<int,std::string>::iterator it = plus.begin();it != plus.end(); ++it)
	//	std::cout << (*it).second << std::endl;

	for(int i = 1;i < 101; i++){
		std::cout << plus[rand()%(45-2)]  << ",";
		if(0 == (i%5)) std::cout << std::endl;
	}

	return 0;
}
